/*
 * harness.c —— wb_vram_dxgi.c 的宿主端（Linux）测试台
 * ============================================================================
 * 为什么要有它：v1 那个 E_NOINTERFACE 的 bug 只在真机上才暴露。这里造一套
 * **假的 DXVK dxgi + 假的适配器对象 + 假的虚表**，把
 *   转发 → EnumAdapters → 虚表校验 → 挂钩 → 改写返回值
 * 整条路径在本机跑通，并且能断言每条安全阀（白名单 / dryrun / 只抬不降 / 动态退回）。
 *
 * 假 dxgi 还刻意复刻了 DXVK 的行为：**只接受"正确的" IID**，别的返回
 * E_NOINTERFACE(0x80004002) —— 这正是 v1 死掉的地方。
 *
 * 编译（见 run.sh）：把被测源码直接 include 进来，并关掉 __declspec/__stdcall，
 * 用 -fshort-wchar 让 L"..." 是 2 字节，跟 WCHAR=u16 对齐。
 */

#include <stdio.h>

#include "../wb_vram_dxgi.c"

/* ======================= 断言 ======================= */

static int g_fail = 0;

#define CHECK(cond, msg) do {                              \
        if (cond) { printf("  ok   %s\n", msg); }          \
        else      { printf("  FAIL %s\n", msg); g_fail++; } \
    } while (0)

/* ======================= 小工具 ======================= */

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static int weq_generic(const char *a, const char *b)
{
    return str_eq(a, b);
}

/* ======================= 假日志（只存内存里） ======================= */

static char  g_logtext[65536];
static size_t g_loglen = 0;
static int    g_log_handle_no = 0;

static void log_push(char c)
{
    if (g_loglen + 2 < sizeof(g_logtext)) { g_logtext[g_loglen++] = c; g_logtext[g_loglen] = 0; }
}

/* WCHAR(u16) -> UTF-8，这样断言里可以直接用中文匹配 */
static void log_append(const WCHAR *s, DWORD n)
{
    DWORD i;
    for (i = 0; i < n; i++) {
        WCHAR c = s[i];
        if (!c) break;
        if (c == '\r') continue;
        if (c == '\n') { log_push('\n'); continue; }
        if (c < 0x80) {
            log_push((char)c);
        } else if (c < 0x800) {
            log_push((char)(0xC0 | (c >> 6)));
            log_push((char)(0x80 | (c & 0x3F)));
        } else {
            log_push((char)(0xE0 | (c >> 12)));
            log_push((char)(0x80 | ((c >> 6) & 0x3F)));
            log_push((char)(0x80 | (c & 0x3F)));
        }
    }
}

static int log_has(const char *needle)
{
    size_t n = 0;
    while (needle[n]) n++;
    for (size_t i = 0; i + n <= g_loglen; i++) {
        size_t k = 0;
        while (k < n && g_logtext[i + k] == needle[k]) k++;
        if (k == n) return 1;
    }
    return 0;
}

/* ======================= 宿主端实现 Win32 那一小撮 ======================= */

static u64 g_tick = 100000;

DWORD WINAPI_ GetCurrentProcessId(void) { return 4242; }
u64   WINAPI_ GetTickCount64(void)      { return g_tick; }
DWORD WINAPI_ GetLastError(void)        { return 0; }

static void wset(WCHAR *d, const char *s)
{
    while (*s) *d++ = (WCHAR)(unsigned char)*s++;
    *d = 0;
}

static int weq(const WCHAR *w, const char *a)
{
    while (*a) {
        if (*w != (WCHAR)(unsigned char)*a) return 0;
        w++; a++;
    }
    return *w == 0;
}

/* ---- 环境变量 ---- */
static const char *g_env_name[16];
static const char *g_env_value[16];
static int         g_env_n = 0;

static void env_set(const char *n, const char *v)
{
    if (g_env_n < 16) { g_env_name[g_env_n] = n; g_env_value[g_env_n] = v; g_env_n++; }
}

DWORD WINAPI_ GetEnvironmentVariableW(const WCHAR *name, WCHAR *buf, DWORD size)
{
    int i;
    for (i = 0; i < g_env_n; i++) {
        if (weq(name, g_env_name[i])) {
            const char *v = g_env_value[i];
            u32 m = 0;
            while (v[m] && m + 1 < size) { buf[m] = (WCHAR)(unsigned char)v[m]; m++; }
            buf[m] = 0;
            return m;
        }
    }
    return 0;
}

/* ---- 文件（带"内容表"，供候选自检读取） ---- */
#define BLOB_MAX 24
static struct { const char *path; const unsigned char *data; size_t len; } g_blobs[BLOB_MAX];
static int g_blob_n = 0;

static void blob_add(const char *path, const void *data, size_t len)
{
    if (g_blob_n < BLOB_MAX) {
        g_blobs[g_blob_n].path = path;
        g_blobs[g_blob_n].data = (const unsigned char *)data;
        g_blobs[g_blob_n].len  = len;
        g_blob_n++;
    }
}

static int blob_index(const WCHAR *wpath)
{
    int i;
    for (i = 0; i < g_blob_n; i++) if (weq(wpath, g_blobs[i].path)) return i;
    return -1;
}

struct FakeHandle { int blob; size_t off; };
static struct FakeHandle g_fh[32];
static int g_fh_n = 0;

static HANDLE new_fh(int blob)
{
    int i;
    for (i = 0; i < g_fh_n; i++)
        if (g_fh[i].blob < 0) { g_fh[i].blob = blob; g_fh[i].off = 0; return (HANDLE)&g_fh[i]; }
    if (g_fh_n >= 32) return INVALID_HANDLE_VALUE_;
    g_fh[g_fh_n].blob = blob;
    g_fh[g_fh_n].off  = 0;
    return (HANDLE)&g_fh[g_fh_n++];
}

static int fh_index(HANDLE h)
{
    int i;
    for (i = 0; i < g_fh_n; i++)
        if ((HANDLE)&g_fh[i] == h && g_fh[i].blob >= 0) return i;
    return -1;
}

HANDLE WINAPI_ CreateFileW(const WCHAR *name, DWORD access, DWORD share,
                           void *sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    (void)share; (void)sa; (void)flags; (void)tmpl;
    if (disp == OPEN_EXISTING_) {
        if (access == GENERIC_READ_) {
            int b = blob_index(name);
            if (b >= 0) return new_fh(b);
        }
        return INVALID_HANDLE_VALUE_;
    }
    g_log_handle_no++;
    return (HANDLE)(ULONG_PTR)(0x7000 + g_log_handle_no);
}

BOOL WINAPI_ WriteFile(HANDLE f, const void *buf, DWORD n, DWORD *written, void *ov)
{
    (void)f; (void)ov;
    log_append((const WCHAR *)buf, n / 2);
    if (written) *written = n;
    return TRUE_;
}

BOOL WINAPI_ ReadFile(HANDLE f, void *buf, DWORD n, DWORD *got, void *ov)
{
    int i = fh_index(f);
    (void)ov;
    if (got) *got = 0;
    if (i < 0) return FALSE_;
    {
        int    b = g_fh[i].blob;
        size_t avail = g_blobs[b].len - g_fh[i].off;
        size_t take  = (n < avail) ? (size_t)n : avail;
        size_t k;
        for (k = 0; k < take; k++)
            ((unsigned char *)buf)[k] = g_blobs[b].data[g_fh[i].off + k];
        g_fh[i].off += take;
        if (got) *got = (DWORD)take;
    }
    return TRUE_;
}

BOOL WINAPI_ CloseHandle(HANDLE h)
{
    int i = fh_index(h);
    if (i >= 0) g_fh[i].blob = -1;   /* 释放槽位（pid_is_self 会频繁开关句柄） */
    return TRUE_;
}

DWORD WINAPI_ GetFileSize(HANDLE h, DWORD *hi)
{
    int i = fh_index(h);
    if (hi) *hi = 0;
    if (i < 0) return 0;
    return (DWORD)g_blobs[g_fh[i].blob].len;
}
BOOL  WINAPI_ DeleteFileW(const WCHAR *n)          { (void)n; return TRUE_; }
DWORD WINAPI_ GetTempPathW(DWORD size, WCHAR *buf)
{
    const char *t = "C:\\users\\steamuser\\AppData\\Local\\Temp\\";
    u32 i = 0;
    while (t[i] && i + 1 < size) { buf[i] = (WCHAR)(unsigned char)t[i]; i++; }
    buf[i] = 0;
    return i;
}

DWORD WINAPI_ GetModuleFileNameW(HMODULE m, WCHAR *buf, DWORD size)
{
    const char *p;
    u32 i = 0;

    if (m == g_self)      p = "C:\\windows\\system32\\dxgi.dll";
    else if (m == 0)      p = "C:\\Games\\GTA\\GTA5_Enhanced.exe";
    else                  p = "C:\\unknown.dll";

    while (p[i] && i + 1 < size) { buf[i] = (WCHAR)(unsigned char)p[i]; i++; }
    buf[i] = 0;
    return i;
}

/* ---- "哪些路径存在" 由场景决定 ---- */
static const char *g_exist[8];
static int         g_exist_n = 0;
static void exist(const char *p) { if (g_exist_n < 8) g_exist[g_exist_n++] = p; }

DWORD WINAPI_ GetFileAttributesW(const WCHAR *name)
{
    int i;
    for (i = 0; i < g_exist_n; i++) if (weq(name, g_exist[i])) return 0x20;
    return INVALID_FILE_ATTRIBUTES_;
}

/* ---- 加载模块：只认识两个 "dll" ---- */
#define MOD_REAL  ((HMODULE)(ULONG_PTR)0x1001)
#define MOD_NVML  ((HMODULE)(ULONG_PTR)0x1002)

static int g_nvml_available = 1;

HMODULE WINAPI_ LoadLibraryW(const WCHAR *name)
{
    if (weq(name, "C:\\windows\\system32\\wbvram_dxvk_dxgi.dll")) return MOD_REAL;
    if (weq(name, "wbvram_dxvk_dxgi.dll"))                        return MOD_REAL;
    if (weq(name, "nvml.dll"))                                    return g_nvml_available ? MOD_NVML : 0;
    if (name[0] == 'Z' && name[1] == ':')                         return MOD_REAL;  /* tool_paths 路线 */
    return 0;
}

BOOL WINAPI_ VirtualProtect(void *addr, SIZE_T size, DWORD prot, DWORD *old)
{
    (void)addr; (void)size; (void)prot;
    if (old) *old = 0x04;
    return TRUE_;
}

/* ---- 假注册表：qwMemorySize = 8192 MiB ---- */
i32 WINAPI_ RegOpenKeyExW(HKEY root, const WCHAR *sub, DWORD opt, DWORD sam, HKEY *out)
{
    (void)root; (void)opt; (void)sam;
    if (weq(sub, "SYSTEM\\CurrentControlSet\\Control\\Video")) { *out = (HKEY)(ULONG_PTR)0x2001; return 0; }
    if (weq(sub, "0000\\0000"))                                 { *out = (HKEY)(ULONG_PTR)0x2002; return 0; }
    *out = 0;
    return 1;
}

i32 WINAPI_ RegEnumKeyExW(HKEY key, DWORD idx, WCHAR *name, DWORD *nlen,
                          DWORD *r, WCHAR *cls, DWORD *cl, void *lw)
{
    (void)key; (void)r; (void)cls; (void)cl; (void)lw; (void)nlen;
    if (idx == 0) { wset(name, "0000"); return 0; }
    return 1;
}

i32 WINAPI_ RegQueryValueExW(HKEY key, const WCHAR *name, DWORD *r, DWORD *type,
                             u8 *data, DWORD *len)
{
    (void)key; (void)r;
    if (weq(name, "HardwareInformation.qwMemorySize")) {
        /* 8192 MiB = 0x200000000 */
        data[0] = 0; data[1] = 0; data[2] = 0; data[3] = 0;
        data[4] = 2; data[5] = 0; data[6] = 0; data[7] = 0;
        if (type) *type = 3;
        if (len)  *len = 8;
        return 0;
    }
    return 1;
}

i32 WINAPI_ RegCloseKey(HKEY k) { (void)k; return 0; }

/* ======================= 假 COM 对象 ======================= */

static void *g_factory_obj[1];
static void *g_factory_vtable[32];
static void *g_adapter_obj[1];
static void *g_adapter_vtable[32];

static u64 g_fake_video_mem = 8160ull << 20;
static u64 g_fake_budget    = 5861ull << 20;
static u64 g_fake_usage     = 5490ull << 20;
static int g_fake_qvmi_calls = 0;

/* 两个"GUID"：只有 factory1 那个是对的，用来复刻 DXVK 的 IID 校验 */
static const char g_iid_factory1[16] = "FACTORY1-FAKE-G";
static const char g_iid_factory[16]  = "FACTORY0-FAKE-G";

static i32 WINAPI_ fake_GetDesc(void *self, DXGI_ADAPTER_DESC_ *out)
{
    (void)self;
    mem_zero(out, sizeof(*out));
    out->VendorId = 0x10de;
    out->DeviceId = 0x2488;
    out->DedicatedVideoMemory = (SIZE_T)g_fake_video_mem;
    out->Description[0] = 'N';
    return 0;
}

static i32 WINAPI_ fake_QueryVideoMemoryInfo(void *self, UINT node, UINT group,
                                             DXGI_QUERY_VIDEO_MEMORY_INFO_ *out)
{
    (void)self; (void)node; (void)group;
    out->Budget = g_fake_budget;
    out->CurrentUsage = g_fake_usage;
    out->AvailableForReservation = 0;
    out->CurrentReservation = 0;
    g_fake_qvmi_calls++;
    return 0;
}

static i32 WINAPI_ fake_EnumAdapters(void *self, UINT idx, void **out)
{
    (void)self;
    if (idx != 0) return 0x887A0002;   /* DXGI_ERROR_NOT_FOUND */
    *out = g_adapter_obj;
    return 0;
}

static i32 WINAPI_ fake_CreateFactory1_maybe(const void *riid, void **out, int which)
{
    if (riid != (const void *)(which ? g_iid_factory1 : g_iid_factory))
        return 0x80004002;             /* E_NOINTERFACE —— 复刻 DXVK 的 IID 校验 */
    *out = g_factory_obj;
    return 0;
}

static i32 WINAPI_ fake_CreateDXGIFactory(const void *riid, void **out)
{
    return fake_CreateFactory1_maybe(riid, out, 0);
}
static i32 WINAPI_ fake_CreateDXGIFactory1(const void *riid, void **out)
{
    return fake_CreateFactory1_maybe(riid, out, 1);
}
static i32 WINAPI_ fake_CreateDXGIFactory2(UINT flags, const void *riid, void **out)
{
    (void)flags;
    return fake_CreateFactory1_maybe(riid, out, 1);
}
static i32 WINAPI_ fake_DeclareRemoval(void) { return 0; }
static i32 WINAPI_ fake_GetDebug1(UINT f, const void *riid, void **out)
{
    (void)f; (void)riid; (void)out; return 0x80004005;
}

/* ---- 假 NVML ---- */
static u64 g_nvml_total = 8192ull << 20;
static u64 g_nvml_used  = 6636ull << 20;
static u64 g_nvml_free  = 1556ull << 20;
static int g_nvml_version_seen = 0;
static int g_nvml_init_calls = 0;

static i32 WINAPI_ nv_init(void)                    { g_nvml_init_calls++; return 0; }
static i32 WINAPI_ nv_count(unsigned *c)            { *c = 1; return 0; }
static i32 WINAPI_ nv_handle(unsigned i, void **d)  { (void)i; *d = (void *)(ULONG_PTR)0x1234; return 0; }

static i32 WINAPI_ nv_mem_v2(void *dev, NVML_MEMORY_V2_ *m)
{
    (void)dev;
    g_nvml_version_seen = (int)m->version;
    m->total = g_nvml_total;
    m->used  = g_nvml_used;
    m->free_ = g_nvml_free;
    return 0;
}
static i32 WINAPI_ nv_mem_v1(void *dev, NVML_MEMORY_V1_ *m)
{
    (void)dev;
    m->total = g_nvml_total;
    m->used  = g_nvml_used;
    m->free_ = g_nvml_free;
    return 0;
}

/* ---- 假的 NVML 进程枚举（wb_vram_dxgi.c 的"剔除自己"数据源）----
 * pid 4321 = 自己（cmdline 含 GTA5_Enhanced.exe，setup/场景里配 blob）；
 * 其余 4 个是"其它进程"，合计 640 MiB。 */
static NVML_PROC_V2_ g_fake_procs[8];
static u32           g_fake_nprocs = 0;
static int           g_procs_bad   = 0;

static void procs_reset(void)
{
    mem_zero(g_fake_procs, sizeof(g_fake_procs));
    g_fake_nprocs = 5;
    g_fake_procs[0].pid = 4321; g_fake_procs[0].used = 500ull << 20;
    g_fake_procs[1].pid = 5001; g_fake_procs[1].used = 250ull << 20;
    g_fake_procs[2].pid = 5002; g_fake_procs[2].used = 180ull << 20;
    g_fake_procs[3].pid = 5003; g_fake_procs[3].used = 120ull << 20;
    g_fake_procs[4].pid = 5004; g_fake_procs[4].used =  90ull << 20;
}

static i32 WINAPI_ nv_gfx_procs(void *dev, unsigned *cnt, NVML_PROC_V2_ *infos)
{
    u32 i;
    (void)dev;
    if (g_procs_bad) return 3;
    for (i = 0; i < g_fake_nprocs; i++) infos[i] = g_fake_procs[i];
    *cnt = g_fake_nprocs;
    return 0;
}
static i32 WINAPI_ nv_cmp_procs(void *dev, unsigned *cnt, NVML_PROC_V2_ *infos)
{
    (void)dev; (void)infos;
    if (g_procs_bad) return 3;
    *cnt = 0;
    return 0;
}

/* /proc/<pid>/cmdline 的替身。with_self=0 时拿掉自己的那条（测"认不出自己"）。
 * 内容用 Windows 路径 —— 实测 Wine 把 GetModuleFileNameW 的结果原样写进 cmdline。 */
static void add_proc_blobs(int with_self)
{
    static const char c_self[] = "C:\\Games\\GTA\\GTA5_Enhanced.exe";
    static const char c_1[]    = "/usr/bin/kwin_wayland";
    static const char c_2[]    = "/usr/bin/plasmashell";
    static const char c_3[]    = "/usr/bin/steam";
    static const char c_4[]    = "/usr/lib/Xorg";
    if (with_self) blob_add("Z:\\proc\\4321\\cmdline", c_self, sizeof(c_self) - 1);
    blob_add("Z:\\proc\\5001\\cmdline", c_1, sizeof(c_1) - 1);
    blob_add("Z:\\proc\\5002\\cmdline", c_2, sizeof(c_2) - 1);
    blob_add("Z:\\proc\\5003\\cmdline", c_3, sizeof(c_3) - 1);
    blob_add("Z:\\proc\\5004\\cmdline", c_4, sizeof(c_4) - 1);
}

void *WINAPI_ GetProcAddress(HMODULE m, const char *name)
{
    if (m == MOD_NVML) {
        if (weq_generic(name, "nvmlInit_v2")) return (void *)nv_init;
        if (weq_generic(name, "nvmlDeviceGetCount_v2")) return (void *)nv_count;
        if (weq_generic(name, "nvmlDeviceGetHandleByIndex_v2")) return (void *)nv_handle;
        if (weq_generic(name, "nvmlDeviceGetMemoryInfo_v2")) return (void *)nv_mem_v2;
        if (weq_generic(name, "nvmlDeviceGetMemoryInfo")) return (void *)nv_mem_v1;
        if (weq_generic(name, "nvmlDeviceGetGraphicsRunningProcesses_v2")) return (void *)nv_gfx_procs;
        if (weq_generic(name, "nvmlDeviceGetComputeRunningProcesses_v2"))  return (void *)nv_cmp_procs;
        return 0;
    }
    if (m == MOD_REAL) {
        if (weq_generic(name, "CreateDXGIFactory")) return (void *)fake_CreateDXGIFactory;
        if (weq_generic(name, "CreateDXGIFactory1")) return (void *)fake_CreateDXGIFactory1;
        if (weq_generic(name, "CreateDXGIFactory2")) return (void *)fake_CreateDXGIFactory2;
        if (weq_generic(name, "DXGIDeclareAdapterRemovalSupport")) return (void *)fake_DeclareRemoval;
        if (weq_generic(name, "DXGIGetDebugInterface1")) return (void *)fake_GetDebug1;
        return 0;
    }
    return 0;
}

/* ======================= 场景 ======================= */

static void setup_objects(void)
{
    int i;
    for (i = 0; i < 32; i++) { g_factory_vtable[i] = 0; g_adapter_vtable[i] = 0; }
    g_factory_obj[0] = g_factory_vtable;
    g_adapter_obj[0] = g_adapter_vtable;

    g_factory_vtable[VTSLOT_EnumAdapters] = (void *)fake_EnumAdapters;

    g_adapter_vtable[VTSLOT_GetDesc]              = (void *)fake_GetDesc;
    g_adapter_vtable[VTSLOT_GetDesc1]             = (void *)fake_GetDesc;
    g_adapter_vtable[VTSLOT_GetDesc2]             = (void *)fake_GetDesc;
    g_adapter_vtable[VTSLOT_GetDesc3]             = (void *)fake_GetDesc;
    g_adapter_vtable[VTSLOT_QueryVideoMemoryInfo] = (void *)fake_QueryVideoMemoryInfo;
}

static u64 desc_video_mem(void)
{
    DXGI_ADAPTER_DESC_ d;
    PFN_GetDesc gd = (PFN_GetDesc)g_adapter_vtable[VTSLOT_GetDesc];
    mem_zero(&d, sizeof(d));
    if (gd(g_adapter_obj, &d) < 0) return 0;
    return (u64)d.DedicatedVideoMemory;
}

static u64 qvmi_budget(void)
{
    DXGI_QUERY_VIDEO_MEMORY_INFO_ m;
    PFN_QueryVideoMemoryInfo q =
        (PFN_QueryVideoMemoryInfo)g_adapter_vtable[VTSLOT_QueryVideoMemoryInfo];
    mem_zero(&m, sizeof(m));
    if (q(g_adapter_obj, 0, 0, &m) < 0) return 0;
    return m.Budget;
}

static void setup_common(void)
{
    DllMain((HMODULE)(ULONG_PTR)0x5000, DLL_PROCESS_ATTACH_, 0);
    exist("C:\\windows\\system32\\wbvram_dxvk_dxgi.dll");
    env_set("WBVRAM_LOG_FILE", "C:\\log.txt");
    procs_reset();          /* cmdline blob 由各场景按需 add_proc_blobs() */
}

int main(int argc, char **argv)
{
    const char *sc = (argc > 1) ? argv[1] : "patch";
    int    hr = 0;
    void  *factory = 0;

    setup_common();
    setup_objects();

    printf("场景: %s\n", sc);

    if (str_eq(sc, "patch")) {
        env_set("SteamGameId", "3240220");
        env_set("WBVRAM_DYNAMIC", "0");   /* 本场景测静态路径，关掉默认的动态枚举 */
        hr = CreateDXGIFactory1(g_iid_factory1, &factory);
        CHECK(hr == 0, "工厂创建成功");
        CHECK(g_adapter_vtable[VTSLOT_GetDesc] == (void *)hook_GetDesc,
              "GetDesc 槽已被挂钩");
        CHECK(g_adapter_vtable[VTSLOT_QueryVideoMemoryInfo] == (void *)hook_QueryVideoMemoryInfo,
              "QueryVideoMemoryInfo 槽已被挂钩");
        CHECK(desc_video_mem() == (8192ull << 20), "GetDesc 报 8192 MiB（原 8160）");
        CHECK(qvmi_budget() == (8192ull - 1536ull) << 20, "预算报 6656 MiB（原 5861）");
        CHECK(log_has("探测通过"), "日志里有探测行");
        CHECK(log_has("已挂钩适配器虚表"), "日志里有挂钩行");
        CHECK(log_has("QueryVideoMemoryInfo(LOCAL): Budget 5861 -> 6656"),
              "日志里有 5861->6656 那行");
    }
    else if (str_eq(sc, "nowhitelist")) {
        env_set("SteamGameId", "999999");
        hr = CreateDXGIFactory1(g_iid_factory1, &factory);
        CHECK(hr == 0, "工厂照常创建（转发不受白名单影响）");
        CHECK(g_adapter_vtable[VTSLOT_GetDesc] == (void *)fake_GetDesc,
              "槽位未被改动");
        CHECK(desc_video_mem() == (8160ull << 20), "GetDesc 保持驱动原值 8160");
        CHECK(qvmi_budget() == (5861ull << 20), "预算保持驱动原值 5861");
        CHECK(log_has("不在白名单"), "日志说明被白名单挡下");
    }
    else if (str_eq(sc, "dryrun")) {
        env_set("SteamGameId", "3240220");
        env_set("WBVRAM_DRYRUN", "1");
        hr = CreateDXGIFactory1(g_iid_factory1, &factory);
        g_tick += 3000;
        CHECK(hr == 0, "工厂创建成功");
        CHECK(g_adapter_vtable[VTSLOT_GetDesc] == (void *)hook_GetDesc,
              "DRYRUN 仍挂钩（为了观测）");
        CHECK(desc_video_mem() == (8160ull << 20), "DRYRUN 不改 GetDesc");
        CHECK(qvmi_budget() == (5861ull << 20), "DRYRUN 不改预算");
        CHECK(log_has("DRYRUN"), "日志标了 DRYRUN");
        CHECK(log_has("[观测]"), "打印了观测行");
        CHECK(log_has("=>"), "观测行带换算（驱动口径其它软件）");
    }
    else if (str_eq(sc, "badriid")) {
        env_set("SteamGameId", "3240220");
        env_set("WBVRAM_DYNAMIC", "0");   /* 本场景测静态路径，关掉默认的动态枚举 */
        /* 故意用错的 IID —— v1 就是死在这上面；v2 必须原样转发错误、不崩 */
        hr = CreateDXGIFactory1(g_iid_factory, &factory);
        CHECK(hr == (int)0x80004002, "错误 IID 的错误码被原样透传");
        CHECK(g_adapter_vtable[VTSLOT_GetDesc] == (void *)fake_GetDesc,
              "没拿到工厂时不瞎挂（未改槽位）");
        /* 正确 IID 的调用随后仍应正常工作 */
        hr = CreateDXGIFactory1(g_iid_factory1, &factory);
        CHECK(hr == 0, "换成正确 IID 后成功");
        CHECK(g_adapter_vtable[VTSLOT_GetDesc] == (void *)hook_GetDesc, "这次挂上了");
        CHECK(qvmi_budget() == (8192ull - 1536ull) << 20, "预算已改写");
    }
    else if (str_eq(sc, "reserve")) {
        env_set("SteamGameId", "3240220");
        env_set("WBVRAM_DYNAMIC", "0");   /* 本场景测静态路径，关掉默认的动态枚举 */
        env_set("WBVRAM_RESERVE_MB", "1000");
        hr = CreateDXGIFactory1(g_iid_factory1, &factory);
        CHECK(hr == 0, "工厂创建成功");
        CHECK(qvmi_budget() == (8192ull - 1000ull) << 20, "余量 1000 => 预算 7192");
    }
    else if (str_eq(sc, "dynamic")) {
        env_set("SteamGameId", "3240220");
        env_set("WBVRAM_DYNAMIC", "1");
        add_proc_blobs(1);               /* cmdline 里含自己的 exe 名 */

        g_fake_budget = 5861ull << 20;   /* 驱动原值，应被我们算的值覆盖 */
        g_fake_usage  = 0;
        g_tick += 1000;
        hr = CreateDXGIFactory1(g_iid_factory1, &factory);
        CHECK(hr == 0, "工厂创建成功");
        CHECK(g_nvml_init_calls == 1, "NVML 只初始化了一次");
        CHECK(desc_video_mem() == (8192ull << 20), "GetDesc 抬到 8192");
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO_ m;
            PFN_QueryVideoMemoryInfo q =
                (PFN_QueryVideoMemoryInfo)g_adapter_vtable[VTSLOT_QueryVideoMemoryInfo];
            mem_zero(&m, sizeof(m));
            q(g_adapter_obj, 0, 0, &m);
            /* 枚举 5 进程，认出自己(4321)剔除 → 其它 = 250+180+120+90 = 640
             * budget = 8192 − 640 − 200 = 7352 */
            CHECK(m.Budget == ((8192ull - 640ull - 200ull) << 20),
                  "8192 − 其它640 − margin200 = 7352");
        }
        CHECK(log_has("除自己外合计=640"), "合计剔掉了自己（640 而非 1140）");
        CHECK(log_has("自己=pid 4321"), "靠 cmdline 认出了自己");

        /* 动态调节：其它进程占用涨 → 额度实时下降 */
        g_fake_procs[1].used = 500ull << 20;   /* 250 → 500，其它合计 890 */
        g_tick += 1000;
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO_ m;
            PFN_QueryVideoMemoryInfo q =
                (PFN_QueryVideoMemoryInfo)g_adapter_vtable[VTSLOT_QueryVideoMemoryInfo];
            mem_zero(&m, sizeof(m));
            q(g_adapter_obj, 0, 0, &m);
            CHECK(m.Budget == ((8192ull - 890ull - 200ull) << 20),
                  "其它进程涨到 890 → 额度实时降到 7102");
        }
        CHECK(log_has("除自己外合计=890"), "第二次枚举反映了新占用");
    }
    else if (str_eq(sc, "default")) {
        /* 一个 WBVRAM_* 都不设 —— 验证默认就是"逐进程枚举、剔除自己"，装完即生效 */
        env_set("SteamGameId", "3240220");
        add_proc_blobs(1);

        g_fake_budget = 5861ull << 20;
        g_fake_usage  = 0;
        g_tick += 1000;
        hr = CreateDXGIFactory1(g_iid_factory1, &factory);
        CHECK(hr == 0, "工厂创建成功");
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO_ m;
            PFN_QueryVideoMemoryInfo q =
                (PFN_QueryVideoMemoryInfo)g_adapter_vtable[VTSLOT_QueryVideoMemoryInfo];
            mem_zero(&m, sizeof(m));
            q(g_adapter_obj, 0, 0, &m);
            CHECK(m.Budget == ((8192ull - 640ull - 200ull) << 20),
                  "不设任何开关也算出 7352");
        }
        CHECK(log_has("[NVML]"), "默认模式的日志里就有 [NVML]");
    }
    else if (str_eq(sc, "noself")) {
        /* cmdline 里找不到自己的 exe 名 → 剔除失败 → others 偏大（保守方向，不是危险方向） */
        env_set("SteamGameId", "3240220");
        env_set("WBVRAM_DYNAMIC", "1");
        add_proc_blobs(0);               /* 只给其它进程的 cmdline */

        g_fake_budget = 5861ull << 20;
        g_fake_usage  = 0;
        g_tick += 1000;
        hr = CreateDXGIFactory1(g_iid_factory1, &factory);
        CHECK(hr == 0, "工厂创建成功");
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO_ m;
            PFN_QueryVideoMemoryInfo q =
                (PFN_QueryVideoMemoryInfo)g_adapter_vtable[VTSLOT_QueryVideoMemoryInfo];
            mem_zero(&m, sizeof(m));
            q(g_adapter_obj, 0, 0, &m);
            /* 剔不掉自己 → others = 500+250+180+120+90 = 1140
             * budget = 8192 − 1140 − 200 = 6852（偏保守，但绝不会再算成 0） */
            CHECK(m.Budget == ((8192ull - 1140ull - 200ull) << 20),
                  "认不出自己 → others=1140、额度 6852（保守）");
        }
        CHECK(log_has("未认出"), "日志明确警告没认出自己");
    }
    else if (str_eq(sc, "noprocs")) {
        /* 枚举接口失败 → 退回静态 reserve（绝不再出现把额度算成 7992 的情况） */
        env_set("SteamGameId", "3240220");
        env_set("WBVRAM_DYNAMIC", "1");
        add_proc_blobs(1);
        g_procs_bad = 1;

        g_fake_budget = 5861ull << 20;
        g_fake_usage  = 0;
        g_tick += 1000;
        hr = CreateDXGIFactory1(g_iid_factory1, &factory);
        CHECK(hr == 0, "工厂创建成功");
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO_ m;
            PFN_QueryVideoMemoryInfo q =
                (PFN_QueryVideoMemoryInfo)g_adapter_vtable[VTSLOT_QueryVideoMemoryInfo];
            mem_zero(&m, sizeof(m));
            q(g_adapter_obj, 0, 0, &m);
            CHECK(m.Budget == ((8192ull - 1536ull) << 20), "枚举失败 → 退回静态 6656");
        }
        CHECK(log_has("无法枚举进程占用"), "日志说明退回了静态");
    }
    else if (str_eq(sc, "nvml-missing")) {
        env_set("SteamGameId", "3240220");
        env_set("WBVRAM_DYNAMIC", "1");
        g_nvml_available = 0;
        hr = CreateDXGIFactory1(g_iid_factory1, &factory);
        CHECK(hr == 0, "工厂创建成功");
        CHECK(qvmi_budget() == (8192ull - 1536ull) << 20, "NVML 不可用 => 退回静态 6656");
        CHECK(log_has("退回静态"), "日志说明了退回原因");
    }
    else if (str_eq(sc, "toolpath")) {
        /* system32 里没有副本，只能靠 STEAM_COMPAT_TOOL_PATHS 自愈 */
        g_exist_n = 0;
        exist("Z:\\home\\u\\Steam\\steamapps\\common\\Proton - Experimental"
              "\\files\\lib\\wine\\dxvk\\x86_64-windows\\wbvram_dxvk_dxgi.dll");
        env_set("SteamGameId", "3240220");
        env_set("WBVRAM_DYNAMIC", "0");   /* 本场景测静态路径，关掉默认的动态枚举 */
        env_set("STEAM_COMPAT_TOOL_PATHS",
                "/home/u/Steam/steamapps/common/Proton - Experimental");
        hr = CreateDXGIFactory1(g_iid_factory1, &factory);
        CHECK(hr == 0, "工厂创建成功");
        CHECK(log_has("STEAM_COMPAT_TOOL_PATHS"), "走的是 tool_paths 自愈路线");
        CHECK(qvmi_budget() == (8192ull - 1536ull) << 20, "预算已改写");
    }
    else if (str_eq(sc, "nested")) {
        /* 复刻线上事故：system32 里那个"原件"其实是我们自己旧版的代理 DLL */
        static unsigned char ours_blob[64];
        static unsigned char clean_blob[64];
        size_t olen = 0, clen = 0;
        const char *s = "wb-vram-shim";        /* 以 UTF-16LE 写进去 */
        while (*s) { ours_blob[olen++] = (unsigned char)*s++; ours_blob[olen++] = 0; }
        clean_blob[clen++] = 'D'; clean_blob[clen++] = 0;
        clean_blob[clen++] = 'X'; clean_blob[clen++] = 0;
        clean_blob[clen++] = 'V'; clean_blob[clen++] = 0;
        clean_blob[clen++] = 'K'; clean_blob[clen++] = 0;

        env_set("SteamGameId", "3240220");
        env_set("WBVRAM_DYNAMIC", "0");   /* 本场景测静态路径，关掉默认的动态枚举 */
        env_set("STEAM_COMPAT_TOOL_PATHS",
                "/home/u/Steam/steamapps/common/Proton - Experimental");
        blob_add("C:\\windows\\system32\\wbvram_dxvk_dxgi.dll", ours_blob, olen);
        blob_add("Z:\\home\\u\\Steam\\steamapps\\common\\Proton - Experimental"
                 "\\files\\lib\\wine\\dxvk\\x86_64-windows\\wbvram_dxvk_dxgi.dll",
                 clean_blob, clen);
        exist("Z:\\home\\u\\Steam\\steamapps\\common\\Proton - Experimental"
              "\\files\\lib\\wine\\dxvk\\x86_64-windows\\wbvram_dxvk_dxgi.dll");

        hr = CreateDXGIFactory1(g_iid_factory1, &factory);
        CHECK(hr == 0, "工厂创建成功");
        CHECK(log_has("候选拒绝"), "拒绝了自己的组件（不再互相转发）");
        CHECK(log_has("STEAM_COMPAT_TOOL_PATHS"), "退到下一候选并成功加载");
        CHECK(g_adapter_vtable[VTSLOT_GetDesc] == (void *)hook_GetDesc, "照常打上补丁");
        CHECK(qvmi_budget() == (8192ull - 1536ull) << 20, "预算已改写");
    }
    else if (str_eq(sc, "all-ours")) {
        /* 最坏情况：所有候选都是我们自己的组件 —— 必须安静放弃，绝不能崩 */
        static unsigned char ours_blob[64];
        size_t olen = 0;
        const char *s = "wbvram_dxvk_dxgi";
        while (*s) { ours_blob[olen++] = (unsigned char)*s++; ours_blob[olen++] = 0; }

        env_set("SteamGameId", "3240220");
        env_set("WBVRAM_DYNAMIC", "0");   /* 本场景测静态路径，关掉默认的动态枚举 */
        g_exist_n = 0;                 /* 只留下 system32 那个（是我们自己的） */
        exist("C:\\windows\\system32\\wbvram_dxvk_dxgi.dll");
        blob_add("C:\\windows\\system32\\wbvram_dxvk_dxgi.dll", ours_blob, olen);

        hr = CreateDXGIFactory1(g_iid_factory1, &factory);
        CHECK(hr == (int)0x80004005, "没有可转发对象时返回 E_FAIL（不崩）");
        CHECK(log_has("候选拒绝"), "明确记了拒绝原因");
        CHECK(log_has("找不到可用的 DXVK 原件"), "明确提示跑 rollback.sh");
        CHECK(g_adapter_vtable[VTSLOT_GetDesc] == (void *)fake_GetDesc, "没有乱改槽位");
    }
    else {
        printf("未知场景\n");
        return 2;
    }

    if (g_fail) {
        printf("\n--- 日志全文 ---\n%s\n", g_logtext);
        printf("失败 %d 项\n", g_fail);
        return 1;
    }
    printf("全部通过\n");
    return 0;
}
