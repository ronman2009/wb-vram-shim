/*
 * wb_vram_shim.c —— 自研「显存计算 + 传递链」（Vulkan explicit layer，纯 C）
 * ============================================================================
 *
 * 目标
 * ----
 * 把 Windows 上的显存预算逻辑搬到 Linux：
 *
 *   Windows 的 VidMm 会算「Total - 其它进程占用 - 系统保留」，把结果当作
 *   DXGI 的 QueryVideoMemoryInfo().Budget 交给游戏，而**不包含请求方自己的占用**
 *   （所以应用的额度不会随着自己分配而缩小，是稳定的）。
 *
 *   Linux 上 NVIDIA 驱动通过 VK_EXT_memory_budget 给出的 heapBudget 明显更保守
 *   （本机实测 8160 的堆只给 5861），DX12 游戏据此自我限流。
 *
 * 本模块独立完成三件事，不依赖、不修改 DXVK / Wine：
 *
 *   1) 计算：用 NVML 读物理总量与「首次查询时刻的全卡已用」，算
 *             可用 = 物理总量 - 已用(其它进程) - 余量
 *   2) 传递：作为 Vulkan 层，在 vkGetPhysicalDeviceMemoryProperties[2] 返回前
 *             重写整张显存表（堆大小 / heapBudget / heapUsage 目标段）
 *   3) 记录：把改写前后的数值完整落日志，便于和 nvidia-smi 对照
 *
 * 为什么挂在 Vulkan 层就够了
 * --------------------------
 *   DXVK、vkd3d-proton、dxvk-nvapi 全都运行在 Linux 侧，最终都通过
 *   libvulkan.so.1 读取内存属性。改这里一次，整条链一起变：
 *
 *     vkGetPhysicalDeviceMemoryProperties2
 *        -> DXVK: DedicatedVideoMemory = 最大 DEVICE_LOCAL 堆大小
 *        -> DXVK: QueryVideoMemoryInfo().Budget = heapBudget
 *        -> vkd3d-proton / dxvk-nvapi: 共用同一份 DXGI 实现
 *        -> 游戏
 *
 *   并且 Wine 的 winevulkan 也是走宿主的 Vulkan loader，所以 Wine 里同样生效。
 *
 * 环境变量
 * --------
 *   WBVRAM_ENABLE       0 关闭（默认 1）
 *   WBVRAM_DRYRUN       1 = 只记录不改写（默认 0）
 *   WBVRAM_MARGIN_MB    余量，默认 200（Windows 侧通常百 MB 级）
 *   WBVRAM_PATCH_HEAP   1 = 把 DEVICE_LOCAL 堆大小改成物理总量（默认 1）
 *                           驱动会给 8192-32=8160，这里改回物理完整值
 *   WBVRAM_PATCH_SHARED 1 = 处理 NON_LOCAL/共享段（默认 1）
 *   WBVRAM_SHARED_MB    >0 时把 NON_LOCAL 堆大小改成该值（默认 0 = 保留驱动值）
 *   WBVRAM_LOG          1 = 同时输出到 stderr（默认 0）
 *   WBVRAM_LOG_FILE     日志路径，默认 $HOME/.cache/wb-vram-shim.log
 *   WBVRAM_REFRESH_SEC  >0 时按此间隔重新计算（只升不降），默认 0 = 首次算完冻结
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan_core.h>

/* ========================================================================
 * 0. Loader 私有结构（Vulkan-Loader 的 vk_layer.h 内容，自己声明，避免依赖）
 * ======================================================================== */

#define WB_STYPE_LOADER_INSTANCE_CREATE_INFO ((VkStructureType)47)
#define WB_LAYER_LINK_INFO                   0u

typedef struct WbLayerInstanceLink {
    struct WbLayerInstanceLink *pNext;
    PFN_vkGetInstanceProcAddr   pfnNextGetInstanceProcAddr;
} WbLayerInstanceLink;

typedef struct WbLayerInstanceCreateInfo {
    VkStructureType sType;
    const void     *pNext;
    uint32_t        function;
    union {
        WbLayerInstanceLink *pLayerInfo;
        PFN_vkVoidFunction   pfnData;
    } u;
} WbLayerInstanceCreateInfo;

/* ========================================================================
 * 1. 配置
 * ======================================================================== */

typedef struct WbConfig {
    int      enable;
    int      dryrun;
    uint64_t margin_bytes;
    int      patch_heap;
    int      patch_shared;
    uint64_t shared_bytes;      /* 0 = 保留驱动值 */
    int      log_stderr;
    char     log_file[512];
    int      refresh_sec;
} WbConfig;

static WbConfig          g_cfg;
static pthread_mutex_t   g_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE             *g_log  = NULL;
static int               g_cfg_done = 0;

static void wb_log(const char *fmt, ...)
{
    char    line[1024];
    va_list ap;
    int     n = 0;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;

    pthread_mutex_lock(&g_lock);
    if (g_log) {
        time_t    t = time(NULL);
        struct tm tm;
        char      ts[32] = "?";
        localtime_r(&t, &tm);
        strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
        fprintf(g_log, "[%s] [pid %d] %s\n", ts, (int)getpid(), line);
        fflush(g_log);
    }
    if (g_cfg.log_stderr) {
        fprintf(stderr, "[wb-vram-shim][pid %d] %s\n", (int)getpid(), line);
        fflush(stderr);
    }
    pthread_mutex_unlock(&g_lock);
}

static uint64_t wb_env_u64(const char *name, uint64_t def)
{
    const char *v = getenv(name);
    if (!v || !*v) return def;
    return (uint64_t)strtoull(v, NULL, 10);
}

static int wb_env_int(const char *name, int def)
{
    const char *v = getenv(name);
    if (!v || !*v) return def;
    return (int)strtol(v, NULL, 10);
}

static void wb_config_init(void)
{
    const char *home;

    if (g_cfg_done) return;
    g_cfg_done = 1;

    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.enable       = wb_env_int("WBVRAM_ENABLE", 1);
    g_cfg.dryrun       = wb_env_int("WBVRAM_DRYRUN", 0);
    g_cfg.margin_bytes = wb_env_u64("WBVRAM_MARGIN_MB", 200) << 20;
    g_cfg.patch_heap   = wb_env_int("WBVRAM_PATCH_HEAP", 1);
    g_cfg.patch_shared = wb_env_int("WBVRAM_PATCH_SHARED", 1);
    g_cfg.shared_bytes = wb_env_u64("WBVRAM_SHARED_MB", 0) << 20;
    g_cfg.log_stderr   = wb_env_int("WBVRAM_LOG", 0);
    g_cfg.refresh_sec  = wb_env_int("WBVRAM_REFRESH_SEC", 0);

    home = getenv("HOME");
    if (home && *home)
        snprintf(g_cfg.log_file, sizeof(g_cfg.log_file), "%s/.cache/wb-vram-shim.log", home);
    else
        snprintf(g_cfg.log_file, sizeof(g_cfg.log_file), "/tmp/wb-vram-shim.log");

    {
        const char *p = getenv("WBVRAM_LOG_FILE");
        if (p && *p) snprintf(g_cfg.log_file, sizeof(g_cfg.log_file), "%s", p);
    }

    g_log = fopen(g_cfg.log_file, "a");
    /* 日志文件打不开不是致命错误，继续（可用 stderr 观察） */
    wb_log("================ wb-vram-shim 载入 ================");
    wb_log("配置: enable=%d dryrun=%d margin=%llu MB heap=%d shared=%d shared_mb=%llu refresh=%ds",
           g_cfg.enable, g_cfg.dryrun,
           (unsigned long long)(g_cfg.margin_bytes >> 20),
           g_cfg.patch_heap, g_cfg.patch_shared,
           (unsigned long long)(g_cfg.shared_bytes >> 20), g_cfg.refresh_sec);
}

/* ========================================================================
 * 2. NVML：显存计算的唯一外部数据源
 * ======================================================================== */

typedef struct {
    unsigned int       version;
    unsigned long long total;
    unsigned long long reserved;
    unsigned long long free;
    unsigned long long used;
} WbNvmlMemoryV2;

typedef struct {
    unsigned int       version;
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
} WbNvmlMemoryV1;

typedef int (*WbFn_init_v2)(void);
typedef int (*WbFn_shutdown)(void);
typedef int (*WbFn_count_v2)(unsigned int *);
typedef int (*WbFn_handle_by_index_v2)(unsigned int, void **);
typedef int (*WbFn_handle_by_uuid)(const char *, void **);
typedef int (*WbFn_get_uuid)(void *, char *);
typedef int (*WbFn_meminfo)(void *, WbNvmlMemoryV1 *);
typedef int (*WbFn_meminfo_v2)(void *, WbNvmlMemoryV2 *);
typedef const char *(*WbFn_error_string)(int);

typedef struct {
    void               *lib;
    int                 inited;
    WbFn_shutdown       shutdown;
    WbFn_count_v2       count;
    WbFn_handle_by_uuid handle_by_uuid;
    WbFn_get_uuid       get_uuid;
    WbFn_meminfo        mem_v1;
    WbFn_meminfo_v2     mem_v2;
    WbFn_error_string   err_str;
} WbNvml;

static WbNvml g_nvml;

/* NVML_STRUCT_VERSION(type, ver) == sizeof(type) | (ver << 24) */
#define WB_NVML_STRUCT_VERSION(t, v) ((unsigned int)(sizeof(t) | ((uint32_t)(v) << 24)))

static int wb_nvml_open(void)
{
    static const char *cands[] = { "libnvidia-ml.so.1", "libnvidia-ml.so" };
    WbFn_init_v2 init_v2 = NULL;
    unsigned int i;

    if (g_nvml.lib) return g_nvml.inited ? 0 : -1;

    for (i = 0; i < sizeof(cands) / sizeof(cands[0]) && !g_nvml.lib; i++)
        g_nvml.lib = dlopen(cands[i], RTLD_NOW | RTLD_LOCAL);

    if (!g_nvml.lib) {
        wb_log("NVML: 无法加载 libnvidia-ml.so.1 (%s) -> 退化为 堆大小-余量", dlerror());
        return -1;
    }

    g_nvml.shutdown      = (WbFn_shutdown)dlsym(g_nvml.lib, "nvmlShutdown");
    g_nvml.count         = (WbFn_count_v2)dlsym(g_nvml.lib, "nvmlDeviceGetCount_v2");
    g_nvml.handle_by_uuid= (WbFn_handle_by_uuid)dlsym(g_nvml.lib, "nvmlDeviceGetHandleByUUID");
    g_nvml.get_uuid      = (WbFn_get_uuid)dlsym(g_nvml.lib, "nvmlDeviceGetUUID");
    g_nvml.mem_v1        = (WbFn_meminfo)dlsym(g_nvml.lib, "nvmlDeviceGetMemoryInfo");
    g_nvml.mem_v2        = (WbFn_meminfo_v2)dlsym(g_nvml.lib, "nvmlDeviceGetMemoryInfo_v2");
    g_nvml.err_str       = (WbFn_error_string)dlsym(g_nvml.lib, "nvmlErrorString");

    init_v2 = (WbFn_init_v2)dlsym(g_nvml.lib, "nvmlInit_v2");
    if (!init_v2) init_v2 = (WbFn_init_v2)dlsym(g_nvml.lib, "nvmlInit");

    if (!init_v2 || !g_nvml.count || !g_nvml.mem_v1) {
        wb_log("NVML: 缺少必要符号 -> 退化为 堆大小-余量");
        dlclose(g_nvml.lib);
        g_nvml.lib = NULL;
        return -1;
    }

    if (init_v2() != 0) {
        wb_log("NVML: nvmlInit 失败 -> 退化为 堆大小-余量");
        dlclose(g_nvml.lib);
        g_nvml.lib = NULL;
        return -1;
    }

    g_nvml.inited = 1;
    wb_log("NVML: 已初始化 (mem_v2=%s)", g_nvml.mem_v2 ? "可用" : "不可用");
    return 0;
}

static void wb_nvml_close(void)
{
    if (g_nvml.lib && g_nvml.inited && g_nvml.shutdown)
        g_nvml.shutdown();
    if (g_nvml.lib)
        dlclose(g_nvml.lib);
    memset(&g_nvml, 0, sizeof(g_nvml));
}

/* Vulkan 的 16 字节 deviceUUID -> "bded5fdb57442cb607f26cb57ef9f446" 小写 hex */
static void wb_uuid_hex(const uint8_t uuid[16], char out[33])
{
    static const char h[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 16; i++) {
        out[i * 2]     = h[(uuid[i] >> 4) & 0xF];
        out[i * 2 + 1] = h[uuid[i] & 0xF];
    }
    out[32] = '\0';
}

/* "GPU-bded5fdb-5744-2cb6-07f2-6cb57ef9f446" -> 去掉前缀与短横线，转小写 */
static void wb_nvml_uuid_norm(const char *in, char out[64])
{
    size_t o = 0;
    if (!in) { out[0] = '\0'; return; }
    if (strncmp(in, "GPU-", 4) == 0) in += 4;
    for (; *in && o < 63; in++) {
        if (*in == '-') continue;
        out[o++] = (char)((*in >= 'A' && *in <= 'Z') ? (*in - 'A' + 'a') : *in);
    }
    out[o] = '\0';
}

/* ========================================================================
 * 3. 每块 GPU 的状态与「显存计算」
 * ======================================================================== */

#define WB_MAX_GPUS 8

typedef struct WbGpu {
    VkPhysicalDevice phys;
    int              resolved;
    int              nvml_ok;
    uint64_t         phys_total;     /* NVML total：物理显存 */
    uint64_t         used_snapshot;  /* 首次计算时的全卡 used（≈ 其它进程占用） */
    uint64_t         free_snapshot;
    uint64_t         reserved;       /* NVML 报告的驱动保留 */
    uint64_t         budget;         /* 我们对外声明的可用上限 */
    uint64_t         last_refresh;
    char             uuid[64];
    char             nvml_name[96];
} WbGpu;

static WbGpu g_gpus[WB_MAX_GPUS];
static int   g_gpu_count = 0;

static WbGpu *wb_gpu_slot(VkPhysicalDevice pd)
{
    int i;
    for (i = 0; i < g_gpu_count; i++)
        if (g_gpus[i].phys == pd) return &g_gpus[i];
    if (g_gpu_count >= WB_MAX_GPUS) return NULL;
    memset(&g_gpus[g_gpu_count], 0, sizeof(g_gpus[g_gpu_count]));
    g_gpus[g_gpu_count].phys = pd;
    return &g_gpus[g_gpu_count++];
}

/*
 * 核心计算：
 *
 *   可用 = 物理总量 - 其它进程占用 - 余量
 *
 * 「其它进程占用」取首次查询时刻的全卡 used：此刻请求方还没开始大额分配，
 * 这个值基本就是桌面 / Steam / wine 附属进程的占用，语义与 Windows 的
 * 「Total - 其它应用占用」一致。之后按 WBVRAM_REFRESH_SEC 刷新时**只升不降**，
 * 保证应用看到的额度不会因为自己分配而被缩水。
 *
 * 若 NVML 不可用，退化为：可用 = DEVICE_LOCAL 堆大小 - 余量。
 */
static void wb_gpu_resolve(WbGpu *g, const VkPhysicalDeviceMemoryProperties *props)
{
    uint64_t fallback_heap = 0;
    uint32_t i;

    if (props) {
        for (i = 0; i < props->memoryHeapCount; i++) {
            if (props->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                if (props->memoryHeaps[i].size > fallback_heap)
                    fallback_heap = props->memoryHeaps[i].size;
        }
    }
    if (!fallback_heap) fallback_heap = 8192ull << 20; /* 兜底 */

    if (!g->nvml_ok && wb_nvml_open() == 0) {
        void *dev = NULL;
        int   rc;
        char  nv_uuid[64] = { 0 };

        if (g->uuid[0] && g_nvml.handle_by_uuid) {
            /* Vulkan 的 32 位 hex -> NVML 期望的 "GPU-8-4-4-4-12" 形式 */
            snprintf(nv_uuid, sizeof(nv_uuid), "GPU-%.8s-%.4s-%.4s-%.4s-%.12s",
                     g->uuid, g->uuid + 8, g->uuid + 12, g->uuid + 16, g->uuid + 20);
            rc = g_nvml.handle_by_uuid(nv_uuid, &dev);
            if (rc == 0 && dev && g_nvml.get_uuid) {
                /* 反向校验一次，确认真的对上了这块卡 */
                char got[96] = { 0 }, norm[64] = { 0 };
                g_nvml.get_uuid(dev, got);
                wb_nvml_uuid_norm(got, norm);
                if (strcmp(norm, g->uuid) != 0) {
                    wb_log("NVML: UUID 校验不一致 (nvml=%s vulkan=%s)，改用设备 0", norm, g->uuid);
                    dev = NULL;
                    rc  = -1;
                }
            }
        } else {
            rc = -1;
        }

        if (rc != 0 && !dev) {
            /* 退化为第 0 块，并如实记录 */
            WbFn_handle_by_index_v2 hbi =
                (WbFn_handle_by_index_v2)dlsym(g_nvml.lib, "nvmlDeviceGetHandleByIndex_v2");
            unsigned int cnt = 0;
            if (hbi && g_nvml.count(&cnt) == 0 && cnt > 0) {
                if (hbi(0, &dev) == 0)
                    wb_log("NVML: 按 UUID 未命中，退化为设备 0（共 %u 块）", cnt);
                else
                    dev = NULL;
            }
        }

        if (!dev) {
            wb_log("NVML: 拿不到设备句柄 -> 使用兜底值");
        } else {
            WbNvmlMemoryV1 m1;
            uint64_t total = 0, used = 0, freeb = 0, reserved = 0;

            if (g_nvml.get_uuid)
                g_nvml.get_uuid(dev, g->nvml_name);
            if (!g->nvml_name[0]) snprintf(g->nvml_name, sizeof(g->nvml_name), "(未知)");

            if (g_nvml.mem_v2) {
                WbNvmlMemoryV2 m2;
                memset(&m2, 0, sizeof(m2));
                m2.version = WB_NVML_STRUCT_VERSION(WbNvmlMemoryV2, 2);
                if (g_nvml.mem_v2(dev, &m2) == 0) {
                    total = m2.total; used = m2.used; freeb = m2.free; reserved = m2.reserved;
                }
            }
            if (!total && g_nvml.mem_v1) {
                memset(&m1, 0, sizeof(m1));
                if (g_nvml.mem_v1(dev, &m1) == 0) {
                    total = m1.total; used = m1.used; freeb = m1.free;
                }
            }

            if (total) {
                g->nvml_ok        = 1;
                g->phys_total     = total;
                g->used_snapshot  = used;
                g->free_snapshot  = freeb;
                g->reserved       = reserved;
            } else {
                wb_log("NVML: 读不到内存信息 -> 使用兜底值");
            }
        }
    }

    if (g->nvml_ok) {
        uint64_t avail = (g->phys_total > g->used_snapshot + g_cfg.margin_bytes)
                       ? (g->phys_total - g->used_snapshot - g_cfg.margin_bytes) : 0;
        g->budget = avail;
    } else {
        g->budget = (fallback_heap > g_cfg.margin_bytes)
                  ? (fallback_heap - g_cfg.margin_bytes) : fallback_heap;
    }

    /* 保护：不能低于堆大小的 60%，否则说明快照时刻已在疯跑，宁可保守报原值 */
    if (fallback_heap && g->budget < fallback_heap / 2)
        g->budget = fallback_heap / 2;

    g->last_refresh = (uint64_t)time(NULL);
    g->resolved     = 1;

    wb_log("计算[%s]: 物理=%llu MiB 其它占用快照=%llu MiB 驱动保留=%llu MiB 余量=%llu MiB => 可用=%llu MiB",
           g->nvml_name[0] ? g->nvml_name : "?",
           (unsigned long long)(g->phys_total >> 20),
           (unsigned long long)(g->used_snapshot >> 20),
           (unsigned long long)(g->reserved >> 20),
           (unsigned long long)(g_cfg.margin_bytes >> 20),
           (unsigned long long)(g->budget >> 20));
}

static void wb_gpu_refresh(WbGpu *g)
{
    /* 周期重算时只升不降；实现上就是把新值取 max */
    uint64_t keep = g->budget;

    if (g_cfg.refresh_sec <= 0) return;
    if (g->last_refresh && (uint64_t)time(NULL) - g->last_refresh < (uint64_t)g_cfg.refresh_sec)
        return;

    g->resolved = 0;
    g->nvml_ok  = 0;   /* 强制重新走一次 NVML */
    wb_gpu_resolve(g, NULL);
    if (g->budget < keep) g->budget = keep;
}

/* ========================================================================
 * 4. 传递链：改写整张显存表
 * ======================================================================== */

static void wb_dump_heaps(const char *tag, const VkPhysicalDeviceMemoryProperties *mp,
                          const VkPhysicalDeviceMemoryBudgetPropertiesEXT *bp)
{
    uint32_t i;
    wb_log("--- %s：memoryHeapCount=%u ---", tag, mp->memoryHeapCount);
    for (i = 0; i < mp->memoryHeapCount; i++) {
        wb_log("  heap[%u] %s size=%llu MiB  budget=%s  usage=%s",
               i,
               (mp->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "DEVICE_LOCAL" : "NON_LOCAL  ",
               (unsigned long long)(mp->memoryHeaps[i].size >> 20),
               bp ? "见下" : "n/a",
               bp ? "见下" : "n/a");
        if (bp)
            wb_log("           budget=%llu MiB usage=%llu MiB",
                   (unsigned long long)(bp->heapBudget[i] >> 20),
                   (unsigned long long)(bp->heapUsage[i] >> 20));
    }
}

static void wb_patch(VkPhysicalDevice pd, WbGpu *g,
                     VkPhysicalDeviceMemoryProperties *mp,
                     VkPhysicalDeviceMemoryBudgetPropertiesEXT *bp)
{
    int      changed = 0;
    uint32_t i, local_idx = UINT32_MAX;
    uint64_t local_size = 0;

    (void)pd;
    if (!g_cfg.enable) return;

    for (i = 0; i < mp->memoryHeapCount; i++) {
        if (mp->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            if (mp->memoryHeaps[i].size > local_size) {
                local_size = mp->memoryHeaps[i].size;
                local_idx  = i;
            }
        }
    }

    wb_gpu_refresh(g);

    /* (a) DEVICE_LOCAL 堆大小 -> 物理总量（抹掉驱动那 32 MiB 保留） */
    if (g_cfg.patch_heap && g->nvml_ok && local_idx != UINT32_MAX) {
        uint32_t k;
        for (k = 0; k < mp->memoryHeapCount; k++) {
            if (!(mp->memoryHeaps[k].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
            if (mp->memoryHeaps[k].size != g->phys_total) {
                wb_log("改写 heap[%u].size: %llu MiB -> %llu MiB（物理总量）",
                       k, (unsigned long long)(mp->memoryHeaps[k].size >> 20),
                       (unsigned long long)(g->phys_total >> 20));
                if (!g_cfg.dryrun) mp->memoryHeaps[k].size = g->phys_total;
                changed = 1;
            }
        }
    }

    /* (b) 预算与共享段 */
    if (bp) {
        for (i = 0; i < mp->memoryHeapCount; i++) {
            const int is_local = (mp->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;

            if (is_local) {
                if (bp->heapBudget[i] != g->budget) {
                    wb_log("改写 heap[%u].heapBudget: %llu MiB -> %llu MiB",
                           i, (unsigned long long)(bp->heapBudget[i] >> 20),
                           (unsigned long long)(g->budget >> 20));
                    if (!g_cfg.dryrun) bp->heapBudget[i] = g->budget;
                    changed = 1;
                }
            } else if (g_cfg.patch_shared) {
                if (g_cfg.shared_bytes && mp->memoryHeaps[i].size != g_cfg.shared_bytes) {
                    wb_log("改写 heap[%u].size(NON_LOCAL): %llu MiB -> %llu MiB",
                           i, (unsigned long long)(mp->memoryHeaps[i].size >> 20),
                           (unsigned long long)(g_cfg.shared_bytes >> 20));
                    if (!g_cfg.dryrun) mp->memoryHeaps[i].size = g_cfg.shared_bytes;
                    changed = 1;
                }
                if (bp->heapBudget[i] < mp->memoryHeaps[i].size) {
                    wb_log("改写 heap[%u].heapBudget(NON_LOCAL): %llu MiB -> %llu MiB（不再打折）",
                           i, (unsigned long long)(bp->heapBudget[i] >> 20),
                           (unsigned long long)(mp->memoryHeaps[i].size >> 20));
                    if (!g_cfg.dryrun) bp->heapBudget[i] = mp->memoryHeaps[i].size;
                    changed = 1;
                }
            }
        }
    } else {
        wb_log("注意：调用方没有带 VkPhysicalDeviceMemoryBudgetPropertiesEXT，"
               "本次只改了堆大小，预算无法改写");
    }

    if (changed)
        wb_dump_heaps(g_cfg.dryrun ? "改写后(DRYRUN 未真正写入)" : "改写后", mp, bp);
}

/* ========================================================================
 * 5. 层实现
 * ======================================================================== */

#define WB_MAX_INSTANCES 8

/*
 * 下层的函数指针。loader 的 trampoline 在进程内是全局的，所以登记一次即可；
 * 这里不做 per-instance 表，避免"物理设备上拿不到 instance"的麻烦。
 */
static PFN_vkGetInstanceProcAddr                  g_gipa_next   = NULL;
static PFN_vkGetPhysicalDeviceMemoryProperties    wb_next_gpdmp = NULL;
static PFN_vkGetPhysicalDeviceMemoryProperties2   wb_next_gpdmp2 = NULL;
static PFN_vkGetPhysicalDeviceProperties2         wb_next_gpdprop2 = NULL;
static PFN_vkEnumeratePhysicalDevices             wb_next_enum  = NULL;
static uint32_t                                   g_api_version = VK_API_VERSION_1_0;

/* 取物理设备 UUID，用于和 NVML 的 "GPU-xxxx" 对齐（多卡时不至于认错卡） */
static void wb_gpu_take_uuid(VkPhysicalDevice pd, WbGpu *g)
{
    VkPhysicalDeviceIDProperties id;
    VkPhysicalDeviceProperties2  p2;

    if (g->uuid[0] || !wb_next_gpdprop2) return;
    if (g_api_version < VK_API_VERSION_1_1) return;

    memset(&id, 0, sizeof(id));
    memset(&p2, 0, sizeof(p2));
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &id;
    wb_next_gpdprop2(pd, &p2);

    wb_uuid_hex(id.deviceUUID, g->uuid);
    wb_log("物理设备 UUID=%s", g->uuid);
}

VKAPI_ATTR VkResult VKAPI_CALL wb_EnumeratePhysicalDevices(
        VkInstance instance, uint32_t *count, VkPhysicalDevice *devices)
{
    VkResult r;

    if (!wb_next_enum) return VK_ERROR_INITIALIZATION_FAILED;
    r = wb_next_enum(instance, count, devices);

    if ((r == VK_SUCCESS || r == VK_INCOMPLETE) && devices && count) {
        uint32_t i;
        pthread_mutex_lock(&g_lock);
        for (i = 0; i < *count; i++) wb_gpu_slot(devices[i]);
        wb_log("EnumeratePhysicalDevices: 登记 %u 个物理设备", *count);
        pthread_mutex_unlock(&g_lock);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL wb_GetPhysicalDeviceMemoryProperties(
        VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *out)
{
    WbGpu *g;

    if (!wb_next_gpdmp) return;
    wb_next_gpdmp(pd, out);

    pthread_mutex_lock(&g_lock);
    g = wb_gpu_slot(pd);
    if (g) {
        if (!g->resolved) {
            wb_gpu_take_uuid(pd, g);
            wb_gpu_resolve(g, out);
        }
        wb_dump_heaps("改写前(1.0 调用)", out, NULL);
        wb_patch(pd, g, out, NULL);
    }
    pthread_mutex_unlock(&g_lock);
}

VKAPI_ATTR void VKAPI_CALL wb_GetPhysicalDeviceMemoryProperties2(
        VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *out)
{
    WbGpu *g;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT *bp = NULL;
    const VkBaseOutStructure *b;

    if (!wb_next_gpdmp2) return;
    wb_next_gpdmp2(pd, out);

    for (b = (const VkBaseOutStructure *)out->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT) {
            bp = (VkPhysicalDeviceMemoryBudgetPropertiesEXT *)b;
            break;
        }
    }

    pthread_mutex_lock(&g_lock);
    g = wb_gpu_slot(pd);
    if (g) {
        if (!g->resolved) {
            wb_gpu_take_uuid(pd, g);
            wb_gpu_resolve(g, &out->memoryProperties);
        }
        wb_dump_heaps("改写前(2 调用)", &out->memoryProperties, bp);
        wb_patch(pd, g, &out->memoryProperties, bp);
    }
    pthread_mutex_unlock(&g_lock);
}

VKAPI_ATTR VkResult VKAPI_CALL wb_CreateInstance(
        const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *ac, VkInstance *out)
{
    /* pNext 里的链信息需要原地改写（前移一格），所以这里必须脱掉 const，
     * 这是所有 Vulkan 层的常规做法。 */
    WbLayerInstanceCreateInfo *lic = (WbLayerInstanceCreateInfo *)ci->pNext;
    PFN_vkGetInstanceProcAddr gipa_next = NULL;
    PFN_vkCreateInstance      create_next;
    VkResult                  r;

    wb_config_init();

    while (lic) {
        if (lic->sType == WB_STYPE_LOADER_INSTANCE_CREATE_INFO &&
            lic->function == WB_LAYER_LINK_INFO) {
            gipa_next = lic->u.pLayerInfo->pfnNextGetInstanceProcAddr;
            /* 前进一格，避免链上重复处理 */
            lic->u.pLayerInfo = lic->u.pLayerInfo->pNext;
            break;
        }
        lic = (WbLayerInstanceCreateInfo *)lic->pNext;
    }

    if (!gipa_next) {
        wb_log("CreateInstance: 没拿到下层 vkGetInstanceProcAddr，直接放行");
        /* 拿不到链信息：说明本层没被正确挂上，退回失败让 loader 处理 */
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    create_next = (PFN_vkCreateInstance)gipa_next(NULL, "vkCreateInstance");
    if (!create_next) return VK_ERROR_INITIALIZATION_FAILED;

    r = create_next(ci, ac, out);
    if (r != VK_SUCCESS) return r;

    pthread_mutex_lock(&g_lock);
    g_gipa_next = gipa_next;
    if (!wb_next_gpdmp)   wb_next_gpdmp   = (PFN_vkGetPhysicalDeviceMemoryProperties)
                                gipa_next(*out, "vkGetPhysicalDeviceMemoryProperties");
    if (!wb_next_gpdmp2)  wb_next_gpdmp2  = (PFN_vkGetPhysicalDeviceMemoryProperties2)
                                gipa_next(*out, "vkGetPhysicalDeviceMemoryProperties2");
    if (!wb_next_gpdprop2) wb_next_gpdprop2 = (PFN_vkGetPhysicalDeviceProperties2)
                                gipa_next(*out, "vkGetPhysicalDeviceProperties2");
    if (!wb_next_enum)    wb_next_enum    = (PFN_vkEnumeratePhysicalDevices)
                                gipa_next(*out, "vkEnumeratePhysicalDevices");

    if (ci->pApplicationInfo && ci->pApplicationInfo->apiVersion >= VK_API_VERSION_1_1)
        g_api_version = ci->pApplicationInfo->apiVersion;

    wb_log("CreateInstance: 下级函数 gpdmp=%s gpdmp2=%s gpdprop2=%s enum=%s api=%u.%u",
           wb_next_gpdmp ? "有" : "无", wb_next_gpdmp2 ? "有" : "无",
           wb_next_gpdprop2 ? "有" : "无", wb_next_enum ? "有" : "无",
           VK_API_VERSION_MAJOR(g_api_version), VK_API_VERSION_MINOR(g_api_version));
    pthread_mutex_unlock(&g_lock);

    return r;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL wb_GetInstanceProcAddr(
        VkInstance instance, const char *name);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL wb_GetDeviceProcAddr(
        VkDevice device, const char *name)
{
    (void)device; (void)name;
    /* 不拦截设备级命令，返回 NULL 让 loader 继续往下找 */
    return NULL;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL wb_GetInstanceProcAddr(
        VkInstance instance, const char *name)
{
    if (!name) return NULL;

    if (strcmp(name, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)wb_GetInstanceProcAddr;
    if (strcmp(name, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)wb_GetDeviceProcAddr;
    if (strcmp(name, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)wb_CreateInstance;

    if (strcmp(name, "vkEnumeratePhysicalDevices") == 0)
        return (PFN_vkVoidFunction)wb_EnumeratePhysicalDevices;
    if (strcmp(name, "vkGetPhysicalDeviceMemoryProperties") == 0)
        return (PFN_vkVoidFunction)wb_GetPhysicalDeviceMemoryProperties;
    if (strcmp(name, "vkGetPhysicalDeviceMemoryProperties2") == 0)
        return (PFN_vkVoidFunction)wb_GetPhysicalDeviceMemoryProperties2;

    (void)instance;
    /* 其余一律返回 NULL，loader 会继续链上的下一层 */
    return NULL;
}

/* 层必须导出的符号（配合 -fvisibility=hidden） */
#define WB_EXPORT __attribute__((visibility("default")))

WB_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    return wb_GetInstanceProcAddr(instance, name);
}

WB_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name)
{
    return wb_GetDeviceProcAddr(device, name);
}

/*
 * 构造函数：loader 一旦 dlopen 本库就会跑到这里。
 * 有了这行日志，"层到底有没有被加载"就不需要靠猜。
 */
__attribute__((constructor))
static void wb_init(void)
{
    wb_config_init();
    wb_log("层库已加载，等待 vkCreateInstance（若之后没有 CreateInstance 记录，"
           "说明 loader 在 ICD 扫描阶段就失败了）");
}

__attribute__((destructor))
static void wb_fini(void)
{
    wb_log("================ wb-vram-shim 卸载 ================");
    pthread_mutex_lock(&g_lock);
    if (g_log) { fclose(g_log); g_log = NULL; }
    pthread_mutex_unlock(&g_lock);
    wb_nvml_close();
}
