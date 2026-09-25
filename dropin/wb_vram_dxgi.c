/*
 * wb_vram_dxgi.c —— 丢进游戏根目录的 dxgi.dll（纯 C，PE，自带声明）
 * ============================================================================
 *
 * 形态
 * ----
 * 和 DXVK 一样：一个 PE 文件扔进游戏目录（GTA5_Enhanced.exe 旁边）即可，
 * 不需要装任何东西、不需要改启动项（可选环境变量调参）。
 *
 *   GTA5_Enhanced.exe
 *   dxgi.dll            <- 本文件编译产物
 *   ...（原有文件）
 *
 * 原理
 * ----
 * 1) Windows 的 DLL 搜索顺序里，**主程序所在目录优先于 system32**，
 *    所以本文件会顶替掉 Proton 装在前缀里的 DXVK dxgi.dll。
 * 2) 本 DLL 只做两件事：
 *      a) 把 DXVK 的 dxgi.dll 从 system32 复制到 %TEMP% 下换个名字加载，
 *         然后把自己那 5 个导出函数原样转发过去（DXVK 的 dxgi 只导出 5 个）。
 *      b) 首次转发时，在自己的进程里找到 DXVK 的 IDXGIAdapter 对象的虚表，
 *         把 GetDesc / GetDesc1 / GetDesc2 / GetDesc3 / QueryVideoMemoryInfo
 *         这几个槽位换成我们的版本，调用原实现后改写返回值。
 *    因为改的是 **DXVK 适配器类的虚表**（整个进程只有一份），
 *    所以无论 dxgi.dll 是游戏加载的还是 vkd3d-proton 加载的，
 *    也无论是 IDXGIAdapter 还是 IDXGIAdapter3/4 视角，全都一起生效。
 *
 * 改写的数值（复刻 Windows 侧的账）
 * --------------------------------
 *   total  = HKLM\...\Control\Video\*\0000\HardwareInformation.qwMemorySize
 *            （本机实测 8192 MiB；取自注册表可拿到物理完整值）
 *            取不到时退回  GetDesc 原值 + 32 MiB
 *   budget = total - reserve        reserve 默认 1536 MiB（= 桌面 + Steam + Wine 附属的实测开销）
 *
 * 环境变量（全部可选，不设也能跑）
 * --------------------------------
 *   WBVRAM_ENABLE       0 = 完全放行（默认 1）
 *   WBVRAM_RESERVE_MB   预留余量 MiB，默认 1536
 *   WBVRAM_TOTAL_MB     强制指定物理总量，默认从注册表读
 *   WBVRAM_LOG          1 = 输出一份日志（默认写 <DLL 所在目录>\wbvram.log）
 *   WBVRAM_NOLOG        1 = 不写日志文件
 *
 * 说明：本文件刻意不 include 任何头文件（Windows 头要拖 mingw 的 libc），
 *       所有类型与导入都自己声明，用 clang + binutils 的 pei-x86-64 目标直接出 PE。
 */

/* ======================= 1. 自带的基础类型与导入 ======================= */

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed   int       i32;
typedef signed   long long i64;

typedef u16                WCHAR;
typedef u16                wchar_t_;
typedef int                BOOL;
typedef unsigned int       DWORD;
typedef unsigned int       UINT;
typedef u64                SIZE_T;
typedef u64                ULONG_PTR;
typedef void              *HANDLE;
typedef void              *HMODULE;
typedef void              *HKEY;
typedef void              *PVOID;

#define TRUE_  1
#define FALSE_ 0
#define S_OK_  0
#define E_FAIL_ ((i32)0x80004005)

#define WINAPI_ __stdcall
#define DLL_EXPORT __declspec(dllexport)

/* kernel32 */
__declspec(dllimport) HMODULE WINAPI_ LoadLibraryW(const WCHAR *name);
__declspec(dllimport) void   *WINAPI_ GetProcAddress(HMODULE m, const char *name);
__declspec(dllimport) BOOL    WINAPI_ FreeLibrary(HMODULE m);
__declspec(dllimport) BOOL    WINAPI_ VirtualProtect(void *addr, SIZE_T size, DWORD prot, DWORD *old);
__declspec(dllimport) DWORD   WINAPI_ GetModuleFileNameW(HMODULE m, WCHAR *buf, DWORD size);
__declspec(dllimport) DWORD   WINAPI_ GetTempPathW(DWORD size, WCHAR *buf);
__declspec(dllimport) BOOL    WINAPI_ CopyFileW(const WCHAR *from, const WCHAR *to, BOOL failIfExists);
__declspec(dllimport) HANDLE  WINAPI_ CreateFileW(const WCHAR *name, DWORD access, DWORD share,
                                                  void *sa, DWORD disp, DWORD flags, HANDLE tmpl);
__declspec(dllimport) BOOL    WINAPI_ WriteFile(HANDLE f, const void *buf, DWORD n, DWORD *written, void *ov);
__declspec(dllimport) BOOL    WINAPI_ CloseHandle(HANDLE h);
__declspec(dllimport) DWORD   WINAPI_ GetEnvironmentVariableW(const WCHAR *name, WCHAR *buf, DWORD size);
__declspec(dllimport) DWORD   WINAPI_ GetCurrentProcessId(void);
__declspec(dllimport) DWORD   WINAPI_ GetLastError(void);

/* advapi32 */
__declspec(dllimport) i32  WINAPI_ RegOpenKeyExW(HKEY root, const WCHAR *sub, DWORD opt,
                                                 DWORD sam, HKEY *out);
__declspec(dllimport) i32  WINAPI_ RegEnumKeyExW(HKEY key, DWORD idx, WCHAR *name, DWORD *nameLen,
                                                 DWORD *reserved, WCHAR *cls, DWORD *clsLen, void *lastWrite);
__declspec(dllimport) i32  WINAPI_ RegQueryValueExW(HKEY key, const WCHAR *name, DWORD *reserved,
                                                    DWORD *type, u8 *data, DWORD *dataLen);
__declspec(dllimport) i32  WINAPI_ RegCloseKey(HKEY key);

#define HKEY_LOCAL_MACHINE_ ((HKEY)(ULONG_PTR)0x80000002u)
#define KEY_READ_           0x00020019u
#define REG_BINARY_         3u
#define INVALID_HANDLE_VALUE_ ((HANDLE)(i64)-1)
#define PAGE_READWRITE_     0x04u
#define GENERIC_WRITE_      0x40000000u
#define CREATE_ALWAYS_      2u
#define FILE_ATTRIBUTE_NORMAL_ 0x80u

/* ======================= 2. 极简字符串/数字工具 ======================= */

static u32 w_len(const WCHAR *s)
{
    u32 n = 0;
    while (s && s[n]) n++;
    return n;
}

static WCHAR *w_cat(WCHAR *dst, const WCHAR *src)
{
    while (*src) *dst++ = *src++;
    *dst = 0;
    return dst;
}

static WCHAR *w_num(WCHAR *dst, u64 v, u32 base)
{
    WCHAR tmp[24];
    u32   n = 0;
    if (!v) { *dst++ = '0'; *dst = 0; return dst; }
    while (v) {
        u32 d = (u32)(v % base);
        tmp[n++] = (WCHAR)(d < 10 ? ('0' + d) : ('a' + d - 10));
        v /= base;
    }
    while (n) *dst++ = tmp[--n];
    *dst = 0;
    return dst;
}

/* ======================= 3. 极简日志 ======================= */

static HANDLE   g_log      = INVALID_HANDLE_VALUE_;
static int      g_log_done = 0;
static int      g_log_off  = 0;
static WCHAR    g_log_path[512];

static void log_line(const WCHAR *s)
{
    DWORD n = 0;
    if (g_log == INVALID_HANDLE_VALUE_) return;
    WriteFile(g_log, s, w_len(s) * sizeof(WCHAR), &n, 0);
    WriteFile(g_log, (const WCHAR *)L"\r\n", 4, &n, 0);
}

static WCHAR g_buf[1024];

static void log_open_next_to_dll(void)
{
    DWORD n;
    WCHAR *p;

    if (g_log_done) return;
    g_log_done = 1;

    n = GetModuleFileNameW(0, g_log_path, 500);
    if (!n || n > 500) return;

    /* 去掉文件名，留下目录 */
    p = g_log_path + w_len(g_log_path);
    while (p > g_log_path && p[-1] != '\\' && p[-1] != '/') p--;
    if (p > g_log_path) p--;
    *p = 0;

    w_cat(g_log_path, (const WCHAR *)L"\\wbvram.log");

    g_log = CreateFileW(g_log_path, GENERIC_WRITE_, 1, 0, CREATE_ALWAYS_,
                        FILE_ATTRIBUTE_NORMAL_, 0);
    if (g_log == INVALID_HANDLE_VALUE_) return;

    {
        WCHAR *o = g_buf;
        o = w_cat(o, (const WCHAR *)L"===== wb-vram-dropin (dxgi.dll) pid=");
        o = w_num(o, GetCurrentProcessId(), 10);
        o = w_cat(o, (const WCHAR *)L" =====");
        *o = 0;
        log_line(g_buf);
    }
}

/* ======================= 4. DXGI 侧的最小定义 ======================= */

typedef struct { DWORD LowPart; i32 HighPart; } LUID_;

typedef struct {
    WCHAR  Description[128];
    UINT   VendorId;
    UINT   DeviceId;
    UINT   SubSysId;
    UINT   Revision;
    SIZE_T DedicatedVideoMemory;
    SIZE_T DedicatedSystemMemory;
    SIZE_T SharedSystemMemory;
    LUID_  AdapterLuid;
} DXGI_ADAPTER_DESC_;

/* DESC1 / DESC2 / DESC3 的前缀与 DESC 完全一致，直接按 DESC 访问即可 */

typedef struct {
    u64 Budget;
    u64 CurrentUsage;
    u64 AvailableForReservation;
    u64 CurrentReservation;
} DXGI_QUERY_VIDEO_MEMORY_INFO_;

/* 只用虚表槽位，不声明完整接口 */
static void **vt(void *self) { return *(void ***)self; }

#define VTSLOT_GetDesc                 8
#define VTSLOT_GetDesc1                10
#define VTSLOT_GetDesc2                11
#define VTSLOT_QueryVideoMemoryInfo    14
#define VTSLOT_GetDesc3                16
#define VTSLOT_EnumAdapters            7

typedef i32 (WINAPI_ *PFN_GetDesc)(void *self, DXGI_ADAPTER_DESC_ *out);
typedef i32 (WINAPI_ *PFN_QueryVideoMemoryInfo)(void *self, UINT node,
                                                UINT group, DXGI_QUERY_VIDEO_MEMORY_INFO_ *out);
typedef i32 (WINAPI_ *PFN_EnumAdapters)(void *self, UINT idx, void **out);
typedef i32 (WINAPI_ *PFN_CreateFactory1)(const void *riid, void **out);

/* ======================= 5. 配置与全局状态 ======================= */

static int      g_enabled    = 1;
static u64      g_total      = 0;       /* 物理总量（字节） */
static u64      g_budget     = 0;       /* 要报给应用的预算（字节） */
static u64      g_reserve    = 1536ull << 20;
static u64      g_total_over = 0;
static int      g_init_done  = 0;
static int      g_init_ok    = 0;
static int      g_patched    = 0;
static HMODULE  g_real       = 0;

static PFN_GetDesc              g_orig_getdesc  = 0;
static PFN_QueryVideoMemoryInfo g_orig_qvmi     = 0;

static PFN_CreateFactory1 g_real_factory1 = 0;
static PFN_CreateFactory1 g_real_factory  = 0;

/* ---- 注册表：读 qwMemorySize ---- */
static u64 read_qw_memsize(void)
{
    HKEY video = 0, inst = 0;
    DWORD i;
    u64  value = 0;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE_,
                      (const WCHAR *)L"SYSTEM\\CurrentControlSet\\Control\\Video",
                      0, KEY_READ_, &video) != 0)
        return 0;

    for (i = 0; i < 64; i++) {
        WCHAR name[256];
        DWORD nlen = 256;
        WCHAR sub[320];
        WCHAR *o;

        if (RegEnumKeyExW(video, i, name, &nlen, 0, 0, 0, 0) != 0) break;

        o = w_cat(sub, name);
        o = w_cat(o, (const WCHAR *)L"\\0000");
        *o = 0;

        if (RegOpenKeyExW(video, sub, 0, KEY_READ_, &inst) != 0) continue;

        {
            u8  data[64];
            DWORD size = sizeof(data), type = 0;
            if (RegQueryValueExW(inst, (const WCHAR *)L"HardwareInformation.qwMemorySize",
                                 0, &type, data, &size) == 0 && size >= 8) {
                u32 k;
                for (k = 0; k < 8; k++) value |= ((u64)data[k]) << (8 * k);
            }
        }
        RegCloseKey(inst);
        if (value) break;
    }

    RegCloseKey(video);
    return value;
}

static void read_env(void)
{
    WCHAR b[64];
    DWORD n;

    n = GetEnvironmentVariableW((const WCHAR *)L"WBVRAM_ENABLE", b, 64);
    if (n && n < 64) g_enabled = (b[0] != '0');

    n = GetEnvironmentVariableW((const WCHAR *)L"WBVRAM_RESERVE_MB", b, 64);
    if (n && n < 64) {
        u64 v = 0; DWORD k;
        for (k = 0; k < n; k++) if (b[k] >= '0' && b[k] <= '9') v = v * 10 + (u64)(b[k] - '0');
        if (v) g_reserve = v << 20;
    }

    n = GetEnvironmentVariableW((const WCHAR *)L"WBVRAM_TOTAL_MB", b, 64);
    if (n && n < 64) {
        u64 v = 0; DWORD k;
        for (k = 0; k < n; k++) if (b[k] >= '0' && b[k] <= '9') v = v * 10 + (u64)(b[k] - '0');
        if (v) g_total_over = v << 20;
    }

    n = GetEnvironmentVariableW((const WCHAR *)L"WBVRAM_NOLOG", b, 64);
    if (n && n < 64 && b[0] == '1') g_log_off = 1;
}

/* ======================= 6. 改写函数 ======================= */

static i32 WINAPI_ hook_GetDesc(void *self, DXGI_ADAPTER_DESC_ *out)
{
    i32 hr = g_orig_getdesc(self, out);
    if (hr >= 0 && out && g_total) {
        if (out->DedicatedVideoMemory != g_total) {
            WCHAR *o = g_buf;
            o = w_cat(o, (const WCHAR *)L"GetDesc: DedicatedVideoMemory ");
            o = w_num(o, out->DedicatedVideoMemory >> 20, 10);
            o = w_cat(o, (const WCHAR *)L" -> ");
            o = w_num(o, g_total >> 20, 10);
            o = w_cat(o, (const WCHAR *)L" MiB");
            *o = 0;
            log_line(g_buf);
            out->DedicatedVideoMemory = (SIZE_T)g_total;
        }
    }
    return hr;
}

static i32 WINAPI_ hook_QueryVideoMemoryInfo(void *self, UINT node,
                                            UINT group, DXGI_QUERY_VIDEO_MEMORY_INFO_ *out)
{
    i32 hr = g_orig_qvmi(self, node, group, out);
    /* DXGI_MEMORY_SEGMENT_GROUP_LOCAL == 0；共享段（1）不动 */
    if (hr >= 0 && out && group == 0 && g_budget) {
        if (out->Budget != g_budget) {
            WCHAR *o = g_buf;
            o = w_cat(o, (const WCHAR *)L"QueryVideoMemoryInfo(LOCAL): Budget ");
            o = w_num(o, out->Budget >> 20, 10);
            o = w_cat(o, (const WCHAR *)L" -> ");
            o = w_num(o, g_budget >> 20, 10);
            o = w_cat(o, (const WCHAR *)L" MiB   (usage=");
            o = w_num(o, out->CurrentUsage >> 20, 10);
            o = w_cat(o, (const WCHAR *)L" MiB)");
            *o = 0;
            log_line(g_buf);
            out->Budget = g_budget;
        }
    }
    return hr;
}

/* 把 DXVK 适配器类的虚表槽位换掉（进程内只有一份） */
static void patch_adapter_vtable(void *adapter)
{
    void **v = vt(adapter);
    void *page = (void *)((ULONG_PTR)v & ~(ULONG_PTR)0xFFF);
    DWORD old = 0;

    if (g_patched) return;

    if (!VirtualProtect(page, 0x1000, PAGE_READWRITE_, &old)) {
        log_line((const WCHAR *)L"VirtualProtect 失败，放弃改写虚表");
        return;
    }

    g_orig_getdesc = (PFN_GetDesc)v[VTSLOT_GetDesc];
    g_orig_qvmi    = (PFN_QueryVideoMemoryInfo)v[VTSLOT_QueryVideoMemoryInfo];

    v[VTSLOT_GetDesc]             = (void *)hook_GetDesc;
    v[VTSLOT_GetDesc1]            = (void *)hook_GetDesc;
    v[VTSLOT_GetDesc2]            = (void *)hook_GetDesc;
    v[VTSLOT_GetDesc3]            = (void *)hook_GetDesc;
    v[VTSLOT_QueryVideoMemoryInfo]= (void *)hook_QueryVideoMemoryInfo;

    VirtualProtect(page, 0x1000, old, &old);

    g_patched = 1;

    {
        WCHAR *o = g_buf;
        o = w_cat(o, (const WCHAR *)L"已挂钩适配器虚表: GetDesc=");
        o = w_num(o, (u64)(ULONG_PTR)g_orig_getdesc, 16);
        o = w_cat(o, (const WCHAR *)L" QueryVideoMemoryInfo=");
        o = w_num(o, (u64)(ULONG_PTR)g_orig_qvmi, 16);
        *o = 0;
        log_line(g_buf);
    }
}

/* ======================= 7. 初始化 ======================= */

static void wb_init(void)
{
    WCHAR path[512], tmp[512];
    WCHAR *o;
    DWORD n;

    if (g_init_done) return;
    g_init_done = 1;

    if (!g_log_off) log_open_next_to_dll();
    read_env();

    if (!g_enabled) {
        log_line((const WCHAR *)L"WBVRAM_ENABLE=0，完全放行");
        return;
    }

    /* 1) 把 system32\dxgi.dll（DXVK 的）复制到 %TEMP% 换个名字加载，
     *    避免与本模块同名冲突 */
    n = GetTempPathW(400, tmp);
    if (!n) { log_line((const WCHAR *)L"GetTempPathW 失败"); return; }

    o = tmp + w_len(tmp);
    o = w_cat(o, (const WCHAR *)L"wb_dxgi_real_");
    o = w_num(o, GetCurrentProcessId(), 10);
    o = w_cat(o, (const WCHAR *)L".dll");
    *o = 0;

    w_cat(path, (const WCHAR *)L"C:\\windows\\system32\\dxgi.dll");
    if (!CopyFileW(path, tmp, FALSE_)) {
        log_line((const WCHAR *)L"复制 system32\\dxgi.dll 失败（可能它不是真实文件）");
        return;
    }

    g_real = LoadLibraryW(tmp);
    if (!g_real) {
        WCHAR *p2 = g_buf;
        p2 = w_cat(p2, (const WCHAR *)L"LoadLibrary 失败 err=");
        p2 = w_num(p2, GetLastError(), 10);
        *p2 = 0;
        log_line(g_buf);
        return;
    }

    g_real_factory  = (PFN_CreateFactory1)GetProcAddress(g_real, "CreateDXGIFactory");
    g_real_factory1 = (PFN_CreateFactory1)GetProcAddress(g_real, "CreateDXGIFactory1");
    if (!g_real_factory1 && !g_real_factory) {
        log_line((const WCHAR *)L"真实 dxgi.dll 里找不到 CreateDXGIFactory1");
        return;
    }

    /* 2) 算 total / budget */
    g_total = g_total_over ? g_total_over : read_qw_memsize();
    if (!g_total) {
        log_line((const WCHAR *)L"注册表里没有 qwMemorySize，稍后用 GetDesc 原值 + 32MiB 兜底");
    }

    /* 3) 枚举一个适配器，拿到 DXVK 的适配器类虚表 */
    {
        /* {7b7166ec-21c7-44ae-b21a-e3218c4d1e42} = IDXGIFactory1 */
        static const u8 iid_factory1[16] = {
            0xec, 0x66, 0x71, 0x7b, 0xc7, 0x21, 0xae, 0x44,
            0xb2, 0x1a, 0xe3, 0x21, 0x8c, 0x4d, 0x1e, 0x42
        };
        void *factory = 0;
        i32   hr;

        hr = g_real_factory1 ? g_real_factory1(iid_factory1, &factory)
                             : g_real_factory(iid_factory1, &factory);
        if (hr < 0 || !factory) {
            WCHAR *p2 = g_buf;
            p2 = w_cat(p2, (const WCHAR *)L"CreateDXGIFactory1 失败 hr=");
            p2 = w_num(p2, (u64)(u32)hr, 16);
            *p2 = 0;
            log_line(g_buf);
            return;
        }

        {
            void *adapter = 0;
            PFN_EnumAdapters enum_adapters = (PFN_EnumAdapters)vt(factory)[VTSLOT_EnumAdapters];
            hr = enum_adapters(factory, 0, &adapter);
            if (hr >= 0 && adapter) {
                /* 注册表兜底：用驱动给的堆大小 + 32MiB 还原物理量 */
                if (!g_total) {
                    static DXGI_ADAPTER_DESC_ d;
                    PFN_GetDesc gd = (PFN_GetDesc)vt(adapter)[VTSLOT_GetDesc];
                    if (gd && gd(adapter, &d) >= 0)
                        g_total = (u64)d.DedicatedVideoMemory + (32ull << 20);
                }
                patch_adapter_vtable(adapter);
            } else {
                log_line((const WCHAR *)L"EnumAdapters 失败");
            }
        }
    }

    if (g_total) {
        g_budget = (g_total > g_reserve) ? (g_total - g_reserve) : g_total;
        {
            WCHAR *o2 = g_buf;
            o2 = w_cat(o2, (const WCHAR *)L"计算结果: total=");
            o2 = w_num(o2, g_total >> 20, 10);
            o2 = w_cat(o2, (const WCHAR *)L" MiB  reserve=");
            o2 = w_num(o2, g_reserve >> 20, 10);
            o2 = w_cat(o2, (const WCHAR *)L" MiB  => budget=");
            o2 = w_num(o2, g_budget >> 20, 10);
            o2 = w_cat(o2, (const WCHAR *)L" MiB");
            *o2 = 0;
            log_line(g_buf);
        }
        g_init_ok = 1;
    } else {
        log_line((const WCHAR *)L"拿不到物理总量，保持驱动原值（不改写）");
    }
}

/* ======================= 8. 转发导出 ======================= */

typedef i32 (WINAPI_ *PFN_Factory)(const void *riid, void **out);
typedef i32 (WINAPI_ *PFN_Factory2)(UINT flags, const void *riid, void **out);
typedef i32 (WINAPI_ *PFN_Void)(void);
typedef i32 (WINAPI_ *PFN_Debug1)(UINT flags, const void *riid, void **out);

static i32 WINAPI_ forward_factory(const char *name, const void *riid, void **out)
{
    PFN_Factory f;
    wb_init();
    if (!g_real) return E_FAIL_;
    f = (PFN_Factory)GetProcAddress(g_real, name);
    return f ? f(riid, out) : E_FAIL_;
}

DLL_EXPORT i32 WINAPI_ CreateDXGIFactory(const void *riid, void **out)
{
    return forward_factory("CreateDXGIFactory", riid, out);
}

DLL_EXPORT i32 WINAPI_ CreateDXGIFactory1(const void *riid, void **out)
{
    return forward_factory("CreateDXGIFactory1", riid, out);
}

DLL_EXPORT i32 WINAPI_ CreateDXGIFactory2(UINT flags, const void *riid, void **out)
{
    PFN_Factory2 f;
    wb_init();
    if (!g_real) return E_FAIL_;
    f = (PFN_Factory2)GetProcAddress(g_real, "CreateDXGIFactory2");
    return f ? f(flags, riid, out) : E_FAIL_;
}

DLL_EXPORT i32 WINAPI_ DXGIDeclareAdapterRemovalSupport(void)
{
    PFN_Void f;
    wb_init();
    if (!g_real) return E_FAIL_;
    f = (PFN_Void)GetProcAddress(g_real, "DXGIDeclareAdapterRemovalSupport");
    return f ? f() : E_FAIL_;
}

DLL_EXPORT i32 WINAPI_ DXGIGetDebugInterface1(UINT flags, const void *riid, void **out)
{
    PFN_Debug1 f;
    wb_init();
    if (!g_real) return E_FAIL_;
    f = (PFN_Debug1)GetProcAddress(g_real, "DXGIGetDebugInterface1");
    return f ? f(flags, riid, out) : E_FAIL_;
}

BOOL WINAPI_ DllMain(HMODULE mod, DWORD reason, void *reserved)
{
    (void)mod; (void)reason; (void)reserved;
    return TRUE_;
}
