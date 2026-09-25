/*
 * wb_vram_dxgi.c —— 作为 Proton/Wine「部件」安装的 dxgi.dll（纯 C，PE，自带声明）
 * ============================================================================
 * v2 修订（2026-09-24 晚，由实机日志驱动）
 *
 * v1 的致命 bug：
 *   v1 在 init 阶段自己拿一个**写死的 IID** 去调 CreateDXGIFactory1，而那个值是
 *   {7b7166ec-…} = IDXGIFactory（不是 IDXGIFactory1）。按 DXGI 的约定
 *   CreateDXGIFactory1 只接受 IDXGIFactory1 及其派生接口，于是 DXVK 返回
 *   E_NOINTERFACE(0x80004002) → 适配器枚举失败 → 虚表从未被打补丁 → 数字一个都没改。
 *   （实机日志原文：`CreateDXGIFactory1 失败 hr=80004002`）
 *
 * v2 的做法：**不猜 IID**。转发任何一个 CreateDXGIFactory* 时，先按「调用者自己的
 *   riid」把工厂建出来，再拿这个**真实存在**的工厂去 EnumAdapters(0) 取适配器、
 *   打补丁。零 IID 常量、零试探，逻辑上不可能再吃 E_NOINTERFACE。
 *
 * 其它改动：
 *   - 4 个 GetDesc 槽位在挂钩前逐个用**放大缓冲**探测校验（防止 DXVK 改虚表布局）。
 *   - 只抬高、绝不下调（v1 在驱动值更高时会把它调低）。
 *   - 观测模式 WBVRAM_DRYRUN=1：只记录不改写，并周期性打印观测行。
 *   - 可选动态模式 WBVRAM_DYNAMIC=1：借 Wine 的 NVML 垫片读驱动真实占用，
 *     budget = 物理总量 − 其它进程真实占用 − margin；拿不到 NVML 自动退回静态 reserve。
 *   - 日志改为**追加**并带 pid/宿主进程，能看到同一次运行里的所有进程（v1 用
 *     CREATE_ALWAYS，后启动的进程会把前面的日志冲掉 —— 这也是当初只看到一条记录的原因）。
 *
 * v2.1（2026-09-25 00:0x，游戏起不来之后加的）
 *   - **候选自检**：见 §2.5。解析 DXVK 原件时，凡是内容含我们标记串的文件一律拒绝，
 *     避免"我们的两份代理互相转发直到爆栈"。事故复盘：安装脚本把旧版自己误当成
 *     "官方原件"存成了备份，于是 v2 → v1 → v2 → … 直接把宿主进程搞死。
 *     现在这种情况会明确记一行 `[候选拒绝]`，然后安静地放弃转发，不再崩。
 *
 * v2.2（2026-09-25 10:0x）—— **已废弃**
 *   - 曾试过**从驱动预算反推**"其它进程占用"（WBVRAM_AUTO）。实测日志证明这条路不通：
 *     驱动预算**不随本进程用量等量下降**（游戏 16→2265 MiB 时预算只在 6645→6506 微动），
 *     于是反推出的"其它"越算越小、额度越算越虚高，最后变负触发"放弃介入"，
 *     补丁自己把自己关掉。整套逻辑已删除。
 *
 * v3.0（2026-09-25 11:1x，按用户要求重做）
 *   - 改成**直接枚举各进程的显存占用**（NVML 的 Graphics/Compute RunningProcesses），
 *     剔除本进程后求和：budget = total − others − margin。**不再参考驱动预算。**
 *   - "本进程"靠读 Z:\proc\self\stat 拿 Linux PID 来认（NVML 报的就是 Linux PID）。
 *   - 写入改为**直接覆盖**（去掉"只抬不降"）：报出去的就是我们自己算的那个量。
 *   - 默认开（WBVRAM_DYNAMIC=1），装完即生效，不需要任何启动项。
 *
 * 形态
 * ----
 * 不做游戏目录投放。本文件被安装进 **Proton 自己的 DXVK 目录**：
 *
 *   <Proton>/files/lib/wine/dxvk/x86_64-windows/
 *       dxgi.dll                 <- 本文件编译产物（顶替 DXVK 原来的那份）
 *       wbvram_dxvk_dxgi.dll     <- DXVK 原件的备份，名字不参与 Proton 的复制名单
 *
 * Proton 每次启动都会把 dxvk 目录里的 dll 复制进前缀的 system32
 * （proton 脚本 `for f in dxvkfiles: try_copy(...)`），并且给它们设
 * `WINEDLLOVERRIDES=...;dxgi=n`（n = 只用 native）。
 * 于是我们这份会被 Proton 亲自摆到 C:\windows\system32\dxgi.dll，
 * 由 Windows 加载器按 native 载入 —— 游戏目录一个字节都不用动。
 *
 * 为什么不能"丢进 WINEDLLPATH 当 builtin"
 * ----------------------------------------
 * 因为 `dxgi=n` 只查 native，Wine 连 builtin 目录都不会看；
 * 而且 Proton 把用户的 WINEDLLPATH 追加在末尾（proton 脚本 2065-2067 行），
 * 排在它自己的 builtin 目录之后。所以 builtin 路线在这里是死的。
 *
 * 原理
 * ----
 * 1) 找到 DXVK 原件（见 `resolve_real_dll` 的候选链，具备自愈能力），
 *    用 LoadLibraryW 以别的文件名加载，把 5 个导出函数原样转发过去。
 * 2) 第一次成功建出 DXGI 工厂时，拿它 EnumAdapters(0) 取到 DXVK 的适配器对象，
 *    把其**类的虚表**槽位
 *      GetDesc(8) / GetDesc1(10) / GetDesc2(11) / GetDesc3(16) / QueryVideoMemoryInfo(14)
 *    换成我们的版本：先调用原实现，再改写返回值。
 *    虚表进程内唯一，所以无论 dxgi.dll 是被游戏加载还是被 vkd3d-proton /
 *    dxvk-nvapi 加载，也无论从 IDXGIAdapter 还是 IDXGIAdapter3/4 视角拿适配器，
 *    全都一起生效。
 *
 * 安全阀
 * ------
 * - **白名单**：本文件会装进 Proton，对"用这个 Proton 的所有游戏"都可见。
 *   因此默认只对 appid 3240220（GTA V Enhanced）生效，其余游戏
 *   **一字节都不改**，只做纯转发。要放开设 WBVRAM_ALL=1。
 * - **虚表健全性校验**：挂钩前按原槽位真调一次，数值不合理（DXVK 改了虚表布局 /
 *   拿错对象）就放弃该槽位并记日志，绝不在没验证过的情况下动指针。
 * - WBVRAM_ENABLE=0 整体关闭（仍然正常转发）；WBVRAM_DRYRUN=1 只看不改。
 *
 * 数值口径
 * --------
 *   total = HKLM\...\Control\Video\*\0000\HardwareInformation.qwMemorySize
 *           （本机实测 8192 MiB；注册表里存的是物理完整值）
 *           取不到时退回 GetDesc 原值 + 32 MiB
 *
 *   **默认（WBVRAM_DYNAMIC=1，装完即生效，不需要任何启动项）**：
 *       others = Σ NVML 枚举到的每个进程的显存占用（**剔除本进程**）
 *       budget = total − others − WBVRAM_MARGIN_MB(默认 200)
 *
 *   数据源：NVML 的 nvmlDeviceGetGraphicsRunningProcesses_v2 +
 *           nvmlDeviceGetComputeRunningProcesses_v2 —— 它们直接给出每个进程的显存占用。
 *   每 500ms 重算一次，桌面占用变化会实时反映到额度上（这就是动态调节）。
 *
 *   "本进程"怎么认：读 Z:\proc\self\stat 拿本进程的 Linux PID（NVML 报的就是
 *   Linux PID），从枚举结果里剔掉。Wine 没导出 unix pid，但 Z: 盘指向 /。
 *   拿不到 PID 就退一步：others = NVML.used − 本进程 CurrentUsage。
 *   NVML 完全不可用时退回静态：budget = total − WBVRAM_RESERVE_MB(默认 1536)。
 *
 *   为什么**不**在驱动预算上做减法（上一版就是这么错的）：
 *   实测日志（2026-09-25 10:5x）证明**驱动预算不随本进程用量等量下降** ——
 *   游戏从 16 MiB 涨到 2265 MiB 的过程中，驱动预算只在 6645→6506 之间微动。
 *   所以 `others = total − 驱动预算 − 本进程` 会把"其它"越算越小、额度越算越虚高，
 *   最后变负触发放弃介入，补丁自己把自己关掉。直接从物理总量减去**实测的
 *   其它进程占用**才是对的。
 *
 *   ⚠ 前提：需要 Wine 的 NVML 垫片（nvml.dll + nvml.so）。本机只有 GE-Proton11-7
 *     自带，install.sh 会把它移植过来；移植的 ABI 兼容性**未验证**。
 *
 * 环境变量（全部可选）
 * --------------------------------
 *   WBVRAM_ENABLE       0 = 完全放行（默认 1）
 *   WBVRAM_DRYRUN       1 = 只观测不改写（默认关）
 *   WBVRAM_ALL          1 = 取消 appid 白名单（默认关）
 *   WBVRAM_APPS         白名单，逗号分隔 appid，默认 "3240220"
 *   WBVRAM_DYNAMIC      0 = 关掉 NVML 动态计算、退回静态 reserve（**默认开**）
 *   WBVRAM_MARGIN_MB    安全垫 MiB，默认 200
 *   WBVRAM_RESERVE_MB   静态模式余量 MiB，默认 1536（仅 WBVRAM_DYNAMIC=0 时用）
 *   WBVRAM_TOTAL_MB     强制指定物理总量，默认从注册表读
 *   WBVRAM_REAL_DLL     直接指定 DXVK 原件路径
 *   WBVRAM_LOG_FILE     日志文件路径，默认 %TEMP%\wbvram.log
 *   WBVRAM_NOLOG        1 = 不写日志
 *
 * 说明：本文件刻意不 include 任何头文件（Windows 头会拖来 mingw 的 libc），
 *       所有类型与导入都自己声明，用 clang + binutils 的 pei-x86-64 直接出 PE。
 *       这样也才能被 hosttest/ 的宿主端测试直接 #include（见 winepart/hosttest）。
 */

/* ======================= 1. 自带的基础类型与导入 ======================= */

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed   int       i32;
typedef signed   long long i64;

typedef u16                WCHAR;
typedef int                BOOL;
typedef unsigned int       DWORD;
typedef unsigned int       UINT;
typedef u64                SIZE_T;
typedef u64                ULONG_PTR;
typedef void              *HANDLE;
typedef void              *HMODULE;
typedef void              *HKEY;

#define TRUE_  1
#define FALSE_ 0
#define E_FAIL_ ((i32)0x80004005)

#define WINAPI_ __stdcall
#define DLL_EXPORT __declspec(dllexport)

/* kernel32 */
__declspec(dllimport) HMODULE WINAPI_ LoadLibraryW(const WCHAR *name);
__declspec(dllimport) void   *WINAPI_ GetProcAddress(HMODULE m, const char *name);
__declspec(dllimport) BOOL    WINAPI_ VirtualProtect(void *addr, SIZE_T size, DWORD prot, DWORD *old);
__declspec(dllimport) DWORD   WINAPI_ GetModuleFileNameW(HMODULE m, WCHAR *buf, DWORD size);
__declspec(dllimport) DWORD   WINAPI_ GetTempPathW(DWORD size, WCHAR *buf);
__declspec(dllimport) DWORD   WINAPI_ GetFileAttributesW(const WCHAR *name);
__declspec(dllimport) HANDLE  WINAPI_ CreateFileW(const WCHAR *name, DWORD access, DWORD share,
                                                  void *sa, DWORD disp, DWORD flags, HANDLE tmpl);
__declspec(dllimport) BOOL    WINAPI_ WriteFile(HANDLE f, const void *buf, DWORD n, DWORD *written, void *ov);
__declspec(dllimport) BOOL    WINAPI_ ReadFile(HANDLE f, void *buf, DWORD n, DWORD *got, void *ov);
__declspec(dllimport) BOOL    WINAPI_ CloseHandle(HANDLE h);
__declspec(dllimport) DWORD   WINAPI_ GetFileSize(HANDLE h, DWORD *hi);
__declspec(dllimport) BOOL    WINAPI_ DeleteFileW(const WCHAR *name);
__declspec(dllimport) DWORD   WINAPI_ GetEnvironmentVariableW(const WCHAR *name, WCHAR *buf, DWORD size);
__declspec(dllimport) DWORD   WINAPI_ GetCurrentProcessId(void);
__declspec(dllimport) u64     WINAPI_ GetTickCount64(void);
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
#define INVALID_HANDLE_VALUE_ ((HANDLE)(i64)-1)
#define INVALID_FILE_ATTRIBUTES_ 0xFFFFFFFFu
#define PAGE_READWRITE_     0x04u
#define GENERIC_READ_       0x80000000u
#define FILE_APPEND_DATA_   0x0004u
#define FILE_SHARE_RW_      0x00000003u
#define FILE_SHARE_READ_    0x00000001u
#define NVML_NA_            0xFFFFFFFFFFFFFFFFull   /* NVML_VALUE_NOT_AVAILABLE */
#define WB_NVML_MAXPROC     512

/* 版本标记。故意做得独特 —— 编译器的字符串池化会合并相同后缀，把中文串拆散，
 * 导致在 PE 二进制里搜不到完整的连续序列（实测中文串就是被这么拆散的）。
 * install.sh 的 ver_tag 用这一个 ASCII 串来认版本，不依赖任何中文。 */
static const char WB_TAG[] = "<<wbshim-v31-procenum>>";
#define OPEN_EXISTING_      3u
#define OPEN_ALWAYS_        4u
#define FILE_ATTRIBUTE_NORMAL_ 0x80u
#define DLL_PROCESS_ATTACH_ 1u

#define NVML_OK_ 0
#define NVML_MEMORY_V2_VERSION_ (0x02000024u)   /* sizeof(nvmlMemory_v2_t)|(2<<24) */

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

static void mem_zero(void *p, u32 n)
{
    u8 *b = (u8 *)p;
    while (n--) *b++ = 0;
}

/* "a,b,c" 里是否有整词 tok（逗号分隔，精确匹配） */
static int w_has_token(const WCHAR *list, const WCHAR *tok)
{
    const WCHAR *p = list;
    u32          want = w_len(tok);

    if (!want) return 0;
    while (*p) {
        const WCHAR *s = p;
        u32 n, i;
        int ok = 1;

        while (*p && *p != ',') p++;
        n = (u32)(p - s);
        if (n == want) {
            for (i = 0; i < n; i++) if (s[i] != tok[i]) { ok = 0; break; }
            if (ok) return 1;
        }
        if (*p == ',') p++;
    }
    return 0;
}

/* /home/x/y -> Z:\home\x\y */
static WCHAR *w_from_unix(WCHAR *dst, const WCHAR *unix_path)
{
    WCHAR *o = dst;
    *o++ = 'Z';
    *o++ = ':';
    while (*unix_path) {
        WCHAR c = *unix_path++;
        if (c == '/') c = '\\';
        *o++ = c;
    }
    *o = 0;
    return o;
}

static int path_exists(const WCHAR *p)
{
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES_;
}

/* ============== 2.5 自有标记自检（防止代理互相转发） ==============
 *
 * 血的事实：2026-09-25 00:0x，游戏起不来。原因是安装脚本把我们**自己 v1 的 DLL**
 * 当成了"官方原件"备份下来，于是链变成
 *      v2(system32\dxgi.dll) → v1(system32\wbvram_dxvk_dxgi.dll) → 又回到 v2 …
 * 两条代理互相转发，栈溢出，宿主进程直接死。
 *
 * 所以这里加一道内容自检：我们的任何版本都含 UTF-16 标记串 "wb-vram-shim"
 * （官方 DXVK 的 dxgi.dll 没有），凡是带标记的候选一律拒绝，绝不链到自己人身上。
 */
static const WCHAR *const g_markers[] = {
    (const WCHAR *)L"wb-vram-shim",
    (const WCHAR *)L"wb-vram-dropin",
    (const WCHAR *)L"wbvram_dxvk_dxgi"
};
#define MARKER_MAX_WCHARS 20

static u32 buf_find_marker(const u8 *buf, u32 bytes)
{
    u32 k;

    for (k = 0; k < (u32)(sizeof(g_markers) / sizeof(g_markers[0])); k++) {
        const WCHAR *m = g_markers[k];
        u32 mlen = w_len(m);
        u32 i;

        if (!mlen || mlen > MARKER_MAX_WCHARS) continue;
        for (i = 0; i + 2 * mlen <= bytes; i += 2) {
            u32 j;
            for (j = 0; j < mlen; j++) {
                u32 c = (u32)buf[i + 2 * j] | ((u32)buf[i + 2 * j + 1] << 8);
                if (c != (u32)m[j]) break;
            }
            if (j == mlen) return 1;
        }
    }
    return 0;
}

/* 分块扫描，块间留重叠，读不到就当"不是我们的"（不阻塞正常流程） */
static int file_has_our_marker(const WCHAR *path)
{
    static u8 buf[64 * 1024];
    HANDLE    f;
    DWORD     got;
    u32       keep = 0;
    int       found = 0;
    int       guard = 0;

    f = CreateFileW(path, GENERIC_READ_, FILE_SHARE_RW_, 0,
                    OPEN_EXISTING_, FILE_ATTRIBUTE_NORMAL_, 0);
    if (f == INVALID_HANDLE_VALUE_) return 0;

    while (!found && guard++ < 1024) {
        u32 total;

        got = 0;
        if (!ReadFile(f, buf + keep, (DWORD)(sizeof(buf) - keep), &got, 0) || !got) break;

        total = keep + got;
        if (buf_find_marker(buf, total)) { found = 1; break; }

        keep = 2 * MARKER_MAX_WCHARS;
        if (keep > total) keep = total;
        if (keep) {
            u32 i;
            for (i = 0; i < keep; i++) buf[i] = buf[total - keep + i];
        }
    }

    CloseHandle(f);
    return found;
}

/* ======================= 3. 日志（追加，带 pid） ======================= */

static HANDLE   g_log      = INVALID_HANDLE_VALUE_;
static int      g_log_done = 0;
static int      g_log_off  = 0;
static HMODULE  g_self     = 0;      /* 本模块句柄，DllMain 里记下；日志要用 */
static WCHAR    g_buf[1024];

static void log_line(const WCHAR *s)
{
    DWORD n = 0;
    if (g_log == INVALID_HANDLE_VALUE_) return;
    WriteFile(g_log, s, w_len(s) * sizeof(WCHAR), &n, 0);
    WriteFile(g_log, (const WCHAR *)L"\r\n", 4, &n, 0);
}

static void log_kv(const WCHAR *label, const WCHAR *value)
{
    WCHAR *o = g_buf;
    o = w_cat(o, label);
    o = w_cat(o, (const WCHAR *)L" ");
    o = w_cat(o, value);
    *o = 0;
    log_line(g_buf);
}

static void log_path(const WCHAR *label, const WCHAR *fe, const WCHAR *path)
{
    WCHAR *o = g_buf;
    o = w_cat(o, label);
    if (fe && fe[0]) {
        o = w_cat(o, (const WCHAR *)L" ");
        o = w_cat(o, fe);
    }
    o = w_cat(o, (const WCHAR *)L" ");
    o = w_cat(o, path);
    *o = 0;
    log_line(g_buf);
}

static void log_open(void)
{
    static WCHAR path[512];
    DWORD n;
    WCHAR *o;

    if (g_log_done) return;
    g_log_done = 1;

    n = GetEnvironmentVariableW((const WCHAR *)L"WBVRAM_LOG_FILE", path, 500);
    if (!n || n >= 500) {
        n = GetTempPathW(400, path);
        if (!n) return;
        o = path + w_len(path);
        o = w_cat(o, (const WCHAR *)L"wbvram.log");
        *o = 0;
    }

    /* 超过 1 MiB 就重来，免得日志无限涨 */
    {
        HANDLE h = CreateFileW(path, GENERIC_READ_, FILE_SHARE_RW_, 0,
                               OPEN_EXISTING_, FILE_ATTRIBUTE_NORMAL_, 0);
        if (h != INVALID_HANDLE_VALUE_) {
            DWORD hi = 0, lo = GetFileSize(h, &hi);
            CloseHandle(h);
            if (hi || lo > (1024u * 1024u)) DeleteFileW(path);
        }
    }

    g_log = CreateFileW(path, FILE_APPEND_DATA_, FILE_SHARE_RW_, 0,
                        OPEN_ALWAYS_, FILE_ATTRIBUTE_NORMAL_, 0);
    if (g_log == INVALID_HANDLE_VALUE_) return;

    {
        static WCHAR me[512];
        static WCHAR host[512];
        WCHAR *o2 = g_buf;

        o2 = w_cat(o2, (const WCHAR *)L"===== wb-vram-shim v3 (Wine 部件版 dxgi.dll) pid=");
        o2 = w_num(o2, GetCurrentProcessId(), 10);
        o2 = w_cat(o2, (const WCHAR *)L" =====");
        *o2 = 0;
        log_line(g_buf);

        me[0] = 0;
        n = GetModuleFileNameW(g_self, me, 500);
        if (n && n < 500) log_path((const WCHAR *)L"本模块:", (const WCHAR *)L"", me);

        host[0] = 0;
        n = GetModuleFileNameW(0, host, 500);
        if (n && n < 500) log_path((const WCHAR *)L"宿主进程:", (const WCHAR *)L"", host);
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

static void **vt(void *self) { return *(void ***)self; }

#define VTSLOT_EnumAdapters            7
#define VTSLOT_GetDesc                 8
#define VTSLOT_GetDesc1                10
#define VTSLOT_GetDesc2                11
#define VTSLOT_QueryVideoMemoryInfo    14
#define VTSLOT_GetDesc3                16

typedef i32 (WINAPI_ *PFN_GetDesc)(void *self, DXGI_ADAPTER_DESC_ *out);
typedef i32 (WINAPI_ *PFN_QueryVideoMemoryInfo)(void *self, UINT node,
                                                UINT group, DXGI_QUERY_VIDEO_MEMORY_INFO_ *out);
typedef i32 (WINAPI_ *PFN_EnumAdapters)(void *self, UINT idx, void **out);
typedef i32 (WINAPI_ *PFN_Factory)(const void *riid, void **out);
typedef i32 (WINAPI_ *PFN_Factory2)(UINT flags, const void *riid, void **out);
typedef i32 (WINAPI_ *PFN_Void)(void);
typedef i32 (WINAPI_ *PFN_Debug1)(UINT flags, const void *riid, void **out);

/* ======================= 5. 配置与全局状态 ======================= */

static int      g_enabled     = 1;
static int      g_apply_all   = 0;
static int      g_dryrun      = 0;
static int      g_dynamic     = 1;          /* 默认开：NVML 枚举进程自己算（需垫片） */
static u64      g_total       = 0;          /* 物理总量（字节） */
static u64      g_budget      = 0;          /* 要报给应用的预算（字节） */
static u64      g_reserve     = 1536ull << 20;
static u64      g_margin      = 200ull << 20;   /* 安全垫（用户指定 200 MiB） */
static u64      g_total_over  = 0;
static int      g_init_done   = 0;
static int      g_gate_ok     = 0;
static int      g_patched     = 0;
static int      g_patch_tries = 0;
static HMODULE  g_real        = 0;
static u64      g_last_usage  = 0;          /* 本进程最近一次 CurrentUsage */
static u64      g_tick_observe = 0;
static u64      g_tick_nvml    = 0;
static u64      g_budget_last  = 0xFFFFFFFFFFFFFFFFull;  /* 上次算出的值，只在变化时写日志 */
static u64      g_tick_wlog    = 0;          /* 写入行日志节流：最多 10s 一条 */
static int      g_nonvml_logged = 0;
static u32      g_self_pid    = 0;   /* 自己的 Linux PID（认到一次就固定） */
static int      g_self_warned = 0;

static WCHAR    g_apps[192];

static PFN_GetDesc              g_orig_getdesc = 0;
static PFN_QueryVideoMemoryInfo g_orig_qvmi    = 0;

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

static void apps_default(void)
{
    g_apps[0] = 0;
    w_cat(g_apps, (const WCHAR *)L"3240220");
}

static u64 env_num(const WCHAR *name)
{
    WCHAR b[64];
    DWORD n = GetEnvironmentVariableW(name, b, 64);
    u64   v = 0;
    DWORD k;

    if (!n || n >= 64) return 0;
    for (k = 0; k < n; k++) if (b[k] >= '0' && b[k] <= '9') v = v * 10 + (u64)(b[k] - '0');
    return v;
}

static void read_env(void)
{
    WCHAR b[192];
    DWORD n;

    /* WBVRAM_ENABLE 默认 1（置 0 才关） */
    g_enabled = 1;
    n = GetEnvironmentVariableW((const WCHAR *)L"WBVRAM_ENABLE", b, 8);
    if (n && n < 8) g_enabled = (b[0] != '0');

    g_apply_all = 0;
    n = GetEnvironmentVariableW((const WCHAR *)L"WBVRAM_ALL", b, 8);
    if (n && n < 8) g_apply_all = (b[0] != '0');

    g_dryrun = 0;
    n = GetEnvironmentVariableW((const WCHAR *)L"WBVRAM_DRYRUN", b, 8);
    if (n && n < 8) g_dryrun = (b[0] != '0');

    /* **默认开启** —— 装完启动游戏就生效，不需要写任何启动项。
     * 只有想退回「硬编码余量」的静态模式时才需要 WBVRAM_DYNAMIC=0。 */
    g_dynamic = 1;
    n = GetEnvironmentVariableW((const WCHAR *)L"WBVRAM_DYNAMIC", b, 8);
    if (n && n < 8) g_dynamic = (b[0] != '0');

    g_log_off = 0;
    n = GetEnvironmentVariableW((const WCHAR *)L"WBVRAM_NOLOG", b, 8);
    if (n && n < 8) g_log_off = (b[0] != '0');

    apps_default();
    n = GetEnvironmentVariableW((const WCHAR *)L"WBVRAM_APPS", g_apps, 190);
    if (!n || n >= 190) apps_default();

    g_reserve = env_num((const WCHAR *)L"WBVRAM_RESERVE_MB") << 20;
    if (!g_reserve) g_reserve = 1536ull << 20;

    g_margin = env_num((const WCHAR *)L"WBVRAM_MARGIN_MB") << 20;
    if (!g_margin) g_margin = 200ull << 20;

    g_total_over = env_num((const WCHAR *)L"WBVRAM_TOTAL_MB") << 20;
}

/* 默认只对本机 GTA V Enhanced(3240220) 生效 */
static int appid_allowed(void)
{
    WCHAR id[64];
    DWORD n;

    if (g_apply_all) return 1;

    n = GetEnvironmentVariableW((const WCHAR *)L"SteamGameId", id, 60);
    if (!n || n >= 60)
        n = GetEnvironmentVariableW((const WCHAR *)L"SteamAppId", id, 60);

    if (!n || n >= 60) {
        log_line((const WCHAR *)L"拿不到 SteamAppId/SteamGameId，默认不改写"
                               L"（要强制生效设 WBVRAM_ALL=1）");
        return 0;
    }

    if (!w_has_token(g_apps, id)) {
        WCHAR *o = g_buf;
        o = w_cat(o, (const WCHAR *)L"appid ");
        o = w_cat(o, id);
        o = w_cat(o, (const WCHAR *)L" 不在白名单 [");
        o = w_cat(o, g_apps);
        o = w_cat(o, (const WCHAR *)L"]，本进程只做转发");
        *o = 0;
        log_line(g_buf);
        return 0;
    }

    log_path((const WCHAR *)L"白名单命中 appid", id, (const WCHAR *)L"");
    return 1;
}

/* ======================= 6. 定位 DXVK 原件（带自愈） ======================= */

static int try_real(const WCHAR *tag, const WCHAR *path)
{
    HMODULE m;

    if (!path || !path[0]) return 0;

    if (!path_exists(path)) {
        log_path((const WCHAR *)L"[候选未命中]", tag, path);
        return 0;
    }

    /* 关键自检：候选不能是我们自己的组件，否则两个代理会互相转发直到爆栈 */
    if (file_has_our_marker(path)) {
        log_path((const WCHAR *)L"[候选拒绝]", tag,
                 (const WCHAR *)L"—— 这是我们自己的组件，链上去会互相转发");
        {
            WCHAR *o = g_buf;
            o = w_cat(o, (const WCHAR *)L"           ");
            o = w_cat(o, path);
            *o = 0;
            log_line(g_buf);
        }
        return 0;
    }

    m = LoadLibraryW(path);
    if (!m) {
        WCHAR *o = g_buf;
        o = w_cat(o, (const WCHAR *)L"[候选失败] ");
        o = w_cat(o, tag);
        o = w_cat(o, (const WCHAR *)L" err=");
        o = w_num(o, GetLastError(), 10);
        o = w_cat(o, (const WCHAR *)L" ");
        o = w_cat(o, path);
        *o = 0;
        log_line(g_buf);
        return 0;
    }

    if (m == g_self) {
        log_line((const WCHAR *)L"[候选丢弃] 解析到自身（同名模块），试下一个");
        return 0;
    }

    g_real = m;
    log_path((const WCHAR *)L"[已加载 DXVK 原件]", tag, path);
    return 1;
}

static void resolve_real_dll(void)
{
    static WCHAR buf[1024];
    static WCHAR cand[1024];
    DWORD n;

    /* 1) 环境变量直指 */
    n = GetEnvironmentVariableW((const WCHAR *)L"WBVRAM_REAL_DLL", buf, 1000);
    if (n && n < 1000 && try_real((const WCHAR *)L"$WBVRAM_REAL_DLL", buf)) return;

    /* 2) 安装脚本放进前缀 system32 的原件副本（最确定） */
    w_cat(cand, (const WCHAR *)L"C:\\windows\\system32\\wbvram_dxvk_dxgi.dll");
    if (try_real((const WCHAR *)L"system32\\wbvram_dxvk_dxgi.dll", cand)) return;

    /* 3) 顺着 Steam 给的 Proton 目录找（新前缀 / 副本丢了也能自愈） */
    n = GetEnvironmentVariableW((const WCHAR *)L"STEAM_COMPAT_TOOL_PATHS", buf, 1000);
    if (n && n < 1000) {
        const WCHAR *p = buf;
        while (*p) {
            const WCHAR *s = p;
            static WCHAR unixpath[512];
            u32 k = 0;

            while (*p && *p != ':') p++;
            while (s < p && k < 500) unixpath[k++] = *s++;
            unixpath[k] = 0;
            if (*p == ':') p++;
            if (!k) continue;

            {
                WCHAR *o = w_from_unix(cand, unixpath);
                o = w_cat(o, (const WCHAR *)L"\\files\\lib\\wine\\dxvk\\x86_64-windows"
                                          L"\\wbvram_dxvk_dxgi.dll");
                *o = 0;
            }
            if (try_real((const WCHAR *)L"STEAM_COMPAT_TOOL_PATHS", cand)) return;
        }
    }

    /* 4) 交给 Wine 按名找 */
    if (try_real((const WCHAR *)L"按名 wbvram_dxvk_dxgi.dll",
                 (const WCHAR *)L"wbvram_dxvk_dxgi.dll")) return;

    /* 5) 兜底：system32\dxgi.dll（dropin 形态下，那一份才是 DXVK 的） */
    if (try_real((const WCHAR *)L"system32\\dxgi.dll",
                 (const WCHAR *)L"C:\\windows\\system32\\dxgi.dll")) return;

    log_line((const WCHAR *)L"找不到可用的 DXVK 原件，无法转发。");
    log_line((const WCHAR *)L"  若上面每个候选都被「拒绝」，说明它们都是我们自己的组件 ——");
    log_line((const WCHAR *)L"  也就是说这份 Proton 的官方 dxgi.dll 已经丢了。");
    log_line((const WCHAR *)L"  跑 winepart/rollback.sh 恢复（它会从别的 compatdata 前缀里找一份");
    log_line((const WCHAR *)L"  与官方 md5 逐字节一致的 dxgi.dll）。");
}

/* ======================= 7. NVML（可选动态模式） ======================= */

typedef void *nvmlDevice_t_;

/* ⚠ 官方 nvml.h 的字段顺序是 **total, free, used** —— 别写反。
 * 第一版写成 total,used,free，于是把"空闲"当"已用"读，是 others 算成 0 的原因之一。 */
typedef struct { u64 total, free_, used; } NVML_MEMORY_V1_;
typedef struct { u32 version; u64 total, reserved, free_, used; } NVML_MEMORY_V2_;

/* nvmlProcessInfo_v2_t 的手工布局：pid 之后补 4 字节，used 才落在 8 字节边界。 */
typedef struct {
    u32 pid;
    u32 pad0;
    u64 used;
    u32 gpu_instance;
    u32 compute_instance;
} NVML_PROC_V2_;

static struct {
    int           state;      /* 0 未试 / 1 可用 / 2 不可用 */
    int           v2_logged;
    nvmlDevice_t_ dev;
    i32 (WINAPI_ *init_v2)(void);
    i32 (WINAPI_ *count_v2)(unsigned *);
    i32 (WINAPI_ *handle_v2)(unsigned, nvmlDevice_t_ *);
    i32 (WINAPI_ *mem_v2)(nvmlDevice_t_, NVML_MEMORY_V2_ *);
    i32 (WINAPI_ *mem_v1)(nvmlDevice_t_, NVML_MEMORY_V1_ *);
    /* 逐进程枚举：直接给出每个进程的显存占用，是"其它进程"的数据源 */
    i32 (WINAPI_ *gfx_procs)(nvmlDevice_t_, unsigned *, NVML_PROC_V2_ *);
    i32 (WINAPI_ *cmp_procs)(nvmlDevice_t_, unsigned *, NVML_PROC_V2_ *);
} g_nvml;

static int nvml_ready(void)
{
    HMODULE m;
    unsigned cnt = 0;

    if (g_nvml.state == 1) return 1;
    if (g_nvml.state == 2) return 0;
    g_nvml.state = 2;

    m = LoadLibraryW((const WCHAR *)L"nvml.dll");
    if (!m) {
        DWORD e = GetLastError();
        WCHAR *o = g_buf;
        o = w_cat(o, (const WCHAR *)L"NVML 垫片加载失败（nvml.dll），GetLastError=");
        o = w_num(o, e, 10);
        *o = 0;
        log_line(g_buf);
        log_line((const WCHAR *)L"  → 退回静态 reserve");
        log_line((const WCHAR *)L"   126 = 搜索路径里没有它 / 193 = 位数不匹配");
        log_line((const WCHAR *)L"  1114 = unixlib(nvml.so) 起不来 → 跨 Proton 移植的 ABI 不匹配");
        log_line((const WCHAR *)L"     ↑ 这种情况必须让游戏跑 GE-Proton（它自带同源垫片）");
        return 0;
    }

    g_nvml.init_v2   = (i32 (WINAPI_ *)(void))GetProcAddress(m, "nvmlInit_v2");
    g_nvml.count_v2  = (i32 (WINAPI_ *)(unsigned *))GetProcAddress(m, "nvmlDeviceGetCount_v2");
    g_nvml.handle_v2 = (i32 (WINAPI_ *)(unsigned, nvmlDevice_t_ *))
                       GetProcAddress(m, "nvmlDeviceGetHandleByIndex_v2");
    g_nvml.mem_v2    = (i32 (WINAPI_ *)(nvmlDevice_t_, NVML_MEMORY_V2_ *))
                       GetProcAddress(m, "nvmlDeviceGetMemoryInfo_v2");
    g_nvml.mem_v1    = (i32 (WINAPI_ *)(nvmlDevice_t_, NVML_MEMORY_V1_ *))
                       GetProcAddress(m, "nvmlDeviceGetMemoryInfo");
    g_nvml.gfx_procs = (i32 (WINAPI_ *)(nvmlDevice_t_, unsigned *, NVML_PROC_V2_ *))
                       GetProcAddress(m, "nvmlDeviceGetGraphicsRunningProcesses_v2");
    g_nvml.cmp_procs = (i32 (WINAPI_ *)(nvmlDevice_t_, unsigned *, NVML_PROC_V2_ *))
                       GetProcAddress(m, "nvmlDeviceGetComputeRunningProcesses_v2");
    if (!g_nvml.init_v2 || !g_nvml.handle_v2 || (!g_nvml.mem_v2 && !g_nvml.mem_v1)) {
        log_line((const WCHAR *)L"NVML 缺关键导出，退回静态");
        return 0;
    }

    if (g_nvml.init_v2() != NVML_OK_) {
        log_line((const WCHAR *)L"nvmlInit_v2 失败，退回静态");
        return 0;
    }
    if (g_nvml.handle_v2(0, &g_nvml.dev) != NVML_OK_ || !g_nvml.dev) {
        log_line((const WCHAR *)L"取 GPU 句柄失败，退回静态");
        return 0;
    }

    if (g_nvml.count_v2 && g_nvml.count_v2(&cnt) == NVML_OK_) {
        WCHAR *o = g_buf;
        o = w_cat(o, (const WCHAR *)L"NVML 就绪，设备数=");
        o = w_num(o, cnt, 10);
        o = w_cat(o, (const WCHAR *)L"；进程枚举接口=");
        o = w_cat(o, g_nvml.gfx_procs ? (const WCHAR *)L"有" : (const WCHAR *)L"无");
        *o = 0;
        log_line(g_buf);
    } else {
        log_line((const WCHAR *)L"NVML 就绪");
    }

    g_nvml.state = 1;
    return 1;
}

/* 整卡 total / used / free。注意 v1 的字段顺序是 total,free,used。 */
static int nvml_memory(u64 *total, u64 *used, u64 *freeb)
{
    if (!nvml_ready()) return 0;

    if (g_nvml.mem_v2) {
        NVML_MEMORY_V2_ v2;
        mem_zero(&v2, sizeof(v2));
        v2.version = NVML_MEMORY_V2_VERSION_;
        if (g_nvml.mem_v2(g_nvml.dev, &v2) == NVML_OK_) {
            *total = v2.total;
            *used  = v2.used;
            *freeb = v2.free_;
            return 1;
        }
        if (!g_nvml.v2_logged) {          /* 只记一次，别每 500ms 刷一行 */
            g_nvml.v2_logged = 1;
            log_line((const WCHAR *)L"nvmlDeviceGetMemoryInfo_v2 不可用，改用 v1（不影响结果）");
        }
    }

    if (g_nvml.mem_v1) {
        NVML_MEMORY_V1_ v1;
        mem_zero(&v1, sizeof(v1));
        if (g_nvml.mem_v1(g_nvml.dev, &v1) == NVML_OK_) {
            *total = v1.total;
            *freeb = v1.free_;
            *used  = v1.used;
            return 1;
        }
    }
    log_line((const WCHAR *)L"NVML 取显存失败，退回静态 reserve");
    return 0;
}

/* ---- 在 /proc 里认自己 ----
 * Wine 下 `Z:\proc\self` 指向的是 **wineserver** 而不是调用进程（实测：
 * 探针里读 /proc/self/stat 得到 331677(wineserver)，而它的真实 PID 是 331729）。
 * 所以绝不能靠 /proc/self 认自己。
 *
 * 可靠的办法：拿自己的 exe 名（GetModuleFileNameW），再去读每个候选进程的
 * `/proc/<pid>/cmdline` 找同名。实测 Wine 会把 Windows 路径原样写进 cmdline，
 * 且与 GetModuleFileNameW 的结果一致。 */

static int str_has_lc(const char *hay, u32 hn, const char *needle)
{
    u32 nn = 0, i, j;
    while (needle[nn]) nn++;
    if (!nn || hn < nn) return 0;
    for (i = 0; i + nn <= hn; i++) {
        for (j = 0; j < nn; j++) if (hay[i + j] != needle[j]) break;
        if (j == nn) return 1;
    }
    return 0;
}

static const char *self_exe_name(void)
{
    static char name[64];
    static int  done = 0;
    WCHAR       w[600];
    DWORD       n;
    u32         i, s = 0, k = 0;

    if (done) return name;
    done = 1;

    w[0] = 0;
    n = GetModuleFileNameW(0, w, 599);
    if (!n || n >= 599) return name;

    for (i = 0; i < n; i++)
        if (w[i] == '\\' || w[i] == '/') s = i + 1;

    for (i = s; i < n && k < sizeof(name) - 1; i++) {
        u8 c = (u8)(w[i] & 0xFF);
        if (c >= 'A' && c <= 'Z') c = (u8)(c + 32);
        if (!c) break;
        name[k++] = (char)c;
    }
    name[k] = 0;
    return name;
}

static int pid_is_self(u32 pid, const char *myname)
{
    WCHAR  path[64], *p;
    HANDLE f;
    char   b[512];
    DWORD  got = 0;
    u32    i;

    if (!myname || !myname[0] || !pid) return 0;

    p = path; p = w_cat(p, (const WCHAR *)L"Z:\\proc\\");
    p = w_num(p, pid, 10);
    p = w_cat(p, (const WCHAR *)L"\\cmdline");
    f = CreateFileW(path, GENERIC_READ_, FILE_SHARE_RW_, 0,
                    OPEN_EXISTING_, FILE_ATTRIBUTE_NORMAL_, 0);
    if (f == INVALID_HANDLE_VALUE_) {
        p = path; p = w_cat(p, (const WCHAR *)L"Z:\\proc\\");
        p = w_num(p, pid, 10);
        p = w_cat(p, (const WCHAR *)L"\\comm");     /* 退路：comm（15 字节截断） */
        f = CreateFileW(path, GENERIC_READ_, FILE_SHARE_RW_, 0,
                        OPEN_EXISTING_, FILE_ATTRIBUTE_NORMAL_, 0);
    }
    if (f == INVALID_HANDLE_VALUE_) return 0;

    if (!ReadFile(f, b, sizeof(b) - 1, &got, 0)) { CloseHandle(f); return 0; }
    CloseHandle(f);
    if (!got) return 0;
    b[got] = 0;
    for (i = 0; i < got; i++)
        if (b[i] >= 'A' && b[i] <= 'Z') b[i] = (char)(b[i] + 32);
    return str_has_lc(b, got, myname);
}

/* 枚举所有进程的显存占用，求和"除自己以外"的。
 * 自己只认一次，认到之后就不再读 /proc（省掉每 500ms 的文件读）。 */
static int nvml_sum_others(u64 *out_sum, u32 *out_n, u32 *out_na)
{
    static NVML_PROC_V2_ gfx[WB_NVML_MAXPROC];
    static NVML_PROC_V2_ cmp[WB_NVML_MAXPROC];
    unsigned ng = WB_NVML_MAXPROC, nc = WB_NVML_MAXPROC, i;
    const char *me;
    u32 n = 0, na = 0, ngfx = 0, ncmp, j;
    u64 sum = 0;
    int ok = 0;

    if (!nvml_ready()) return 0;
    if (!g_nvml.gfx_procs && !g_nvml.cmp_procs) return 0;

    me = self_exe_name();

    mem_zero(gfx, sizeof(gfx));
    if (g_nvml.gfx_procs && g_nvml.gfx_procs(g_nvml.dev, &ng, gfx) == NVML_OK_) {
        ngfx = (ng < WB_NVML_MAXPROC) ? ng : WB_NVML_MAXPROC;
        ok = 1;
        for (i = 0; i < ngfx; i++) {
            n++;
            if (!gfx[i].pid) continue;
            if (!g_self_pid && pid_is_self(gfx[i].pid, me)) g_self_pid = gfx[i].pid;
            if (gfx[i].pid == g_self_pid) continue;        /* 自己，剔除 */
            if (gfx[i].used == NVML_NA_) { na++; continue; }
            sum += gfx[i].used;
        }
    }

    mem_zero(cmp, sizeof(cmp));
    if (g_nvml.cmp_procs && g_nvml.cmp_procs(g_nvml.dev, &nc, cmp) == NVML_OK_) {
        ncmp = (nc < WB_NVML_MAXPROC) ? nc : WB_NVML_MAXPROC;
        ok = 1;
        for (i = 0; i < ncmp; i++) {
            int dup = 0;
            n++;
            if (!cmp[i].pid) continue;
            if (!g_self_pid && pid_is_self(cmp[i].pid, me)) g_self_pid = cmp[i].pid;
            if (cmp[i].pid == g_self_pid) continue;
            for (j = 0; j < ngfx; j++)
                if (gfx[j].pid == cmp[i].pid) { dup = 1; break; }
            if (dup) continue;                              /* 同一进程别算两遍 */
            if (cmp[i].used == NVML_NA_) { na++; continue; }
            sum += cmp[i].used;
        }
    }

    if (!ok) return 0;
    *out_sum = sum;
    *out_n   = n;
    *out_na  = na;
    return 1;
}

/* ======================= 8. 预算计算 ======================= */

static void compute_budget(void)
{
    u64 others = 0, nb = 0;
    u32 n = 0, na = 0;
    WCHAR *o;

    if (!g_total) return;

    if (!g_dynamic) {                       /* 显式关掉动态 → 静态 reserve */
        g_budget = (g_total > g_reserve) ? (g_total - g_reserve) : g_total;
        return;
    }

    if (!nvml_sum_others(&others, &n, &na)) {
        /* 枚举不可用 → 退回静态 reserve */
        g_budget = (g_total > g_reserve) ? (g_total - g_reserve) : g_total;
        if (!g_nonvml_logged) {
            g_nonvml_logged = 1;
            o = g_buf;
            o = w_cat(o, (const WCHAR *)L"无法枚举进程占用 → 退回静态 reserve => budget=");
            o = w_num(o, g_budget >> 20, 10);
            o = w_cat(o, (const WCHAR *)L" MiB");
            *o = 0;
            log_line(g_buf);
        }
        return;
    }

    /* 用户要的公式：物理总量 − 除本进程外的所有进程占用 − 安全垫 */
    nb = (g_total > others + g_margin) ? (g_total - others - g_margin) : 0;
    if (nb > g_total) nb = g_total;
    g_budget = nb;

    if (g_budget != g_budget_last) {
        u64 whole_u = 0, whole_t = 0, whole_f = 0;

        g_budget_last = g_budget;
        o = g_buf;
        o = w_cat(o, (const WCHAR *)L"[NVML] 枚举进程=");
        o = w_num(o, n, 10);
        o = w_cat(o, (const WCHAR *)L" 个（无用量=");
        o = w_num(o, na, 10);
        o = w_cat(o, (const WCHAR *)L"）除自己外合计=");
        o = w_num(o, others >> 20, 10);
        o = w_cat(o, (const WCHAR *)L" MiB ; margin=");
        o = w_num(o, g_margin >> 20, 10);
        o = w_cat(o, (const WCHAR *)L" => budget=");
        o = w_num(o, nb >> 20, 10);
        o = w_cat(o, (const WCHAR *)L" MiB ; 整卡已用=");
        if (nvml_memory(&whole_t, &whole_u, &whole_f))
            o = w_num(o, whole_u >> 20, 10);
        else
            o = w_cat(o, (const WCHAR *)L"?");
        o = w_cat(o, (const WCHAR *)L" MiB ; 自己=");
        if (g_self_pid) {
            o = w_cat(o, (const WCHAR *)L"pid ");
            o = w_num(o, g_self_pid, 10);
        } else {
            o = w_cat(o, (const WCHAR *)L"★未认出（已剔除 0 个，额度偏保守）");
        }
        *o = 0;
        log_line(g_buf);

        if (!g_self_pid && !g_self_warned) {
            g_self_warned = 1;
            log_line((const WCHAR *)L"  ⚠ 未能在 NVML 进程列表里认出本进程（/proc/<pid>/cmdline 里找不到自己的 exe 名）");
            log_line((const WCHAR *)L"     → 游戏自己占的那部分被算进\"其它\"，额度偏保守（安全方向）");
        }
    }
}

/* ======================= 9. 改写函数 ======================= */

static i32 WINAPI_ hook_GetDesc(void *self, DXGI_ADAPTER_DESC_ *out)
{
    i32 hr = g_orig_getdesc(self, out);

    if (hr >= 0 && out && g_total && !g_dryrun) {
        if ((u64)out->DedicatedVideoMemory < g_total) {
            WCHAR *o = g_buf;
            o = w_cat(o, (const WCHAR *)L"GetDesc: DedicatedVideoMemory ");
            o = w_num(o, (u64)out->DedicatedVideoMemory >> 20, 10);
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

    if (hr < 0 || !out) return hr;
    /* node 0 = 物理 GPU；group 0 = DXGI_MEMORY_SEGMENT_GROUP_LOCAL（共享段不动） */
    if (node != 0 || group != 0) return hr;

    g_last_usage = out->CurrentUsage;

    /* 每 500ms 重新枚举一次进程占用，动态刷新额度（这就是"动态调节"） */
    if (g_dynamic) {
        u64 now = GetTickCount64();
        if (now - g_tick_nvml >= 500) {
            g_tick_nvml = now;
            compute_budget();
        }
    }

    /* 周期性观测行（DRYRUN 或动态模式），便于与 nvtop 实测对照 */
    if (g_dryrun || g_dynamic) {
        u64 now = GetTickCount64();
        if (now - g_tick_observe >= 10000) {
            u64 db  = (u64)out->Budget;
            u64 use = (u64)out->CurrentUsage;
            WCHAR *o = g_buf;

            g_tick_observe = now;
            o = w_cat(o, (const WCHAR *)L"[观测] 驱动预算=");
            o = w_num(o, db >> 20, 10);
            o = w_cat(o, (const WCHAR *)L" 本进程用量=");
            o = w_num(o, use >> 20, 10);
            o = w_cat(o, (const WCHAR *)L" 物理总量=");
            o = w_num(o, g_total >> 20, 10);
            o = w_cat(o, (const WCHAR *)L" MiB ; 我们算的=");
            o = w_num(o, g_budget >> 20, 10);
            o = w_cat(o, (const WCHAR *)L" MiB");
            *o = 0;
            log_line(g_buf);
        }
    }

    /* 注意：驱动每次查询都返回它自己的原值，所以这个条件**每帧都会成立**。
     * v2.4 之前每帧写一行日志 → 一次会话写出 40MB。现在节流到最多 10 秒一条。 */
    if (!g_dryrun && g_budget && (u64)out->Budget != g_budget) {
        u64 now = GetTickCount64();
        WCHAR *o = g_buf;

        if (now - g_tick_wlog >= 10000) {
            g_tick_wlog = now;
            o = w_cat(o, (const WCHAR *)L"QueryVideoMemoryInfo(LOCAL): Budget ");
            o = w_num(o, out->Budget >> 20, 10);
            o = w_cat(o, (const WCHAR *)L" -> ");
            o = w_num(o, g_budget >> 20, 10);
            o = w_cat(o, (const WCHAR *)L" MiB   (usage=");
            o = w_num(o, out->CurrentUsage >> 20, 10);
            o = w_cat(o, (const WCHAR *)L" MiB)");
            *o = 0;
            log_line(g_buf);
        }
        out->Budget = g_budget;
    }
    return hr;
}

/* ======================= 10. 虚表校验与打补丁 ======================= */

/* 用放大缓冲探一次 GetDesc 槽位，避免把过小的结构喂给 GetDesc1/2/3 */
static int probe_desc(void *adapter, int slot, DXGI_ADAPTER_DESC_ *out)
{
    static u8 buf[1024];
    PFN_GetDesc gd = (PFN_GetDesc)vt(adapter)[slot];
    DXGI_ADAPTER_DESC_ *d;

    if (!gd) return 0;
    mem_zero(buf, sizeof(buf));
    if (gd(adapter, (DXGI_ADAPTER_DESC_ *)buf) < 0) return 0;

    d = (DXGI_ADAPTER_DESC_ *)buf;
    if (d->DedicatedVideoMemory < (64ull << 20) ||
        d->DedicatedVideoMemory > (1024ull << 30)) return 0;
    if (!d->VendorId || !d->DeviceId) return 0;

    out->VendorId             = d->VendorId;
    out->DeviceId             = d->DeviceId;
    out->DedicatedVideoMemory = d->DedicatedVideoMemory;
    out->Description[0]       = d->Description[0];
    return 1;
}

static int adapter_sane(void *adapter, int *ok_getdesc3)
{
    DXGI_ADAPTER_DESC_ base, probe;
    DXGI_QUERY_VIDEO_MEMORY_INFO_ mi;
    PFN_QueryVideoMemoryInfo qv;

    *ok_getdesc3 = 0;
    if (!adapter) return 0;

    if (!probe_desc(adapter, VTSLOT_GetDesc, &base)) {
        log_line((const WCHAR *)L"GetDesc(槽8) 探测不通过，放弃改写");
        return 0;
    }

    {
        int slots[3];
        int i;
        slots[0] = VTSLOT_GetDesc1;
        slots[1] = VTSLOT_GetDesc2;
        slots[2] = VTSLOT_GetDesc3;
        for (i = 0; i < 3; i++) {
            if (!probe_desc(adapter, slots[i], &probe)) {
                log_line((const WCHAR *)L"某个 GetDescN 探测不通过，该槽位不挂钩");
                continue;
            }
            if (probe.VendorId != base.VendorId ||
                probe.DedicatedVideoMemory != base.DedicatedVideoMemory) {
                log_line((const WCHAR *)L"某个 GetDescN 与 GetDesc 不一致，该槽位不挂钩");
                continue;
            }
            if (slots[i] == VTSLOT_GetDesc3) *ok_getdesc3 = 1;
        }
    }

    qv = (PFN_QueryVideoMemoryInfo)vt(adapter)[VTSLOT_QueryVideoMemoryInfo];
    if (!qv) {
        log_line((const WCHAR *)L"QueryVideoMemoryInfo 槽位为空，放弃改写");
        return 0;
    }
    mem_zero(&mi, sizeof(mi));
    if (qv(adapter, 0, 0, &mi) < 0) {
        log_line((const WCHAR *)L"QueryVideoMemoryInfo 探测失败，放弃改写");
        return 0;
    }
    if (mi.Budget > (1024ull << 30) || mi.Budget < mi.CurrentUsage) {
        log_line((const WCHAR *)L"预算数值不合理，虚表布局可能已变，放弃改写");
        return 0;
    }

    {
        WCHAR *o = g_buf;
        o = w_cat(o, (const WCHAR *)L"探测通过：DedicatedVideoMemory=");
        o = w_num(o, (u64)base.DedicatedVideoMemory >> 20, 10);
        o = w_cat(o, (const WCHAR *)L" MiB, Budget=");
        o = w_num(o, mi.Budget >> 20, 10);
        o = w_cat(o, (const WCHAR *)L" MiB, Usage=");
        o = w_num(o, mi.CurrentUsage >> 20, 10);
        o = w_cat(o, (const WCHAR *)L" MiB => 驱动口径其它软件=");
        o = w_num(o, ((u64)base.DedicatedVideoMemory > mi.Budget)
                     ? (((u64)base.DedicatedVideoMemory - mi.Budget) >> 20) : 0, 10);
        o = w_cat(o, (const WCHAR *)L" MiB");
        *o = 0;
        log_line(g_buf);
    }

    return 1;
}

static void patch_adapter_vtable(void *adapter)
{
    int    ok3 = 0;
    void **v;
    void  *page;
    DWORD  old = 0;

    if (g_patched) return;
    if (!adapter_sane(adapter, &ok3)) return;

    v    = vt(adapter);
    page = (void *)((ULONG_PTR)v & ~(ULONG_PTR)0xFFF);

    if (!VirtualProtect(page, 0x1000, PAGE_READWRITE_, &old)) {
        log_line((const WCHAR *)L"VirtualProtect 失败，放弃改写虚表");
        return;
    }

    g_orig_getdesc = (PFN_GetDesc)v[VTSLOT_GetDesc];
    g_orig_qvmi    = (PFN_QueryVideoMemoryInfo)v[VTSLOT_QueryVideoMemoryInfo];

    v[VTSLOT_GetDesc]              = (void *)hook_GetDesc;
    v[VTSLOT_GetDesc1]             = (void *)hook_GetDesc;
    v[VTSLOT_GetDesc2]             = (void *)hook_GetDesc;
    if (ok3) v[VTSLOT_GetDesc3]    = (void *)hook_GetDesc;
    v[VTSLOT_QueryVideoMemoryInfo] = (void *)hook_QueryVideoMemoryInfo;

    VirtualProtect(page, 0x1000, old, &old);

    g_patched = 1;

    {
        WCHAR *o = g_buf;
        o = w_cat(o, (const WCHAR *)L"已挂钩适配器虚表: GetDesc=");
        o = w_num(o, (u64)(ULONG_PTR)g_orig_getdesc, 16);
        o = w_cat(o, (const WCHAR *)L" QueryVideoMemoryInfo=");
        o = w_num(o, (u64)(ULONG_PTR)g_orig_qvmi, 16);
        o = w_cat(o, (const WCHAR *)L" GetDesc3=");
        o = w_cat(o, ok3 ? (const WCHAR *)L"挂钩" : (const WCHAR *)L"跳过");
        *o = 0;
        log_line(g_buf);
    }
}

/* 拿到一个**真实存在**的工厂后，直接用它枚举适配器并打补丁（不猜 IID） */
static void patch_from_factory(void *factory)
{
    PFN_EnumAdapters ea;
    void *adapter = 0;
    i32   hr;

    if (g_patched || !g_gate_ok || !g_enabled) return;
    if (!factory) return;
    if (g_patch_tries >= 3) return;
    g_patch_tries++;
    /* 注意：DRYRUN 下**照样挂钩**，但两个钩子只读不改 —— 否则观测行永远打不出来 */

    ea = (PFN_EnumAdapters)vt(factory)[VTSLOT_EnumAdapters];
    if (!ea) {
        log_line((const WCHAR *)L"工厂的 EnumAdapters 槽位为空");
        return;
    }

    hr = ea(factory, 0, &adapter);
    if (hr < 0 || !adapter) {
        WCHAR *o = g_buf;
        o = w_cat(o, (const WCHAR *)L"EnumAdapters(0) 失败 hr=");
        o = w_num(o, (u64)(u32)hr, 16);
        *o = 0;
        log_line(g_buf);
        return;
    }

    /* 注册表兜底：用驱动给的堆大小 + 32 MiB 还原物理量 */
    if (!g_total) {
        DXGI_ADAPTER_DESC_ d;
        PFN_GetDesc gd = (PFN_GetDesc)vt(adapter)[VTSLOT_GetDesc];
        mem_zero(&d, sizeof(d));
        if (gd && gd(adapter, &d) >= 0)
            g_total = (u64)d.DedicatedVideoMemory + (32ull << 20);
    }
    compute_budget();
    patch_adapter_vtable(adapter);
}

/* ======================= 11. 初始化 ======================= */

static void wb_init(void)
{
    if (g_init_done) return;
    g_init_done = 1;

    read_env();
    if (!g_log_off) log_open();

    log_line((const WCHAR *)L"===== 启动 =====");
    {
        WCHAR *o = g_buf;
        const char *p = WB_TAG;
        while (*p) *o++ = (WCHAR)(unsigned char)*p++;
        *o = 0;
        log_line(g_buf);      /* 版本标记：install.sh 靠它认版本 */
    }
    log_kv((const WCHAR *)L"模式:", g_dryrun ? (const WCHAR *)L"DRYRUN（只观测不改写）"
                                             : (const WCHAR *)L"改写");

    /* 1) 找到并加载 DXVK 原件 */
    resolve_real_dll();
    if (!g_real) return;

    if (!GetProcAddress(g_real, "CreateDXGIFactory1") &&
        !GetProcAddress(g_real, "CreateDXGIFactory")) {
        log_line((const WCHAR *)L"DXVK 原件里找不到 CreateDXGIFactory1（原件不对？）");
        return;
    }

    if (!g_enabled) {
        log_line((const WCHAR *)L"WBVRAM_ENABLE=0，只转发不改写");
        return;
    }

    /* 2) 白名单闸门 */
    g_gate_ok = appid_allowed();
    if (!g_gate_ok) return;

    /* 3) 算 total / budget */
    g_total = g_total_over ? g_total_over : read_qw_memsize();
    if (!g_total) {
        log_line((const WCHAR *)L"注册表里没有 qwMemorySize，等第一次 GetDesc 兜底");
        return;
    }
    if (g_dynamic) nvml_ready();     /* 先让 NVML 就绪，再算第一遍 */
    compute_budget();

    {
        WCHAR *o = g_buf;
        o = w_cat(o, (const WCHAR *)L"[结果] total=");
        o = w_num(o, g_total >> 20, 10);
        o = w_cat(o, (const WCHAR *)L" MiB ; 算法=");
        if (g_dynamic)
            o = w_cat(o, (const WCHAR *)L"NVML 枚举各进程占用 → total − 其它 − margin");
        else {
            o = w_cat(o, (const WCHAR *)L"静态 reserve=");
            o = w_num(o, g_reserve >> 20, 10);
            o = w_cat(o, (const WCHAR *)L" MiB");
        }
        o = w_cat(o, (const WCHAR *)L" => budget=");
        o = w_num(o, g_budget >> 20, 10);
        o = w_cat(o, (const WCHAR *)L" MiB");
        *o = 0;
        log_line(g_buf);
    }
}

/* ======================= 12. 转发导出 ======================= */

static i32 WINAPI_ forward_factory(const char *name, const void *riid, void **out)
{
    PFN_Factory f;
    i32         hr;

    wb_init();
    if (!g_real) return E_FAIL_;

    f = (PFN_Factory)GetProcAddress(g_real, name);
    if (!f) return E_FAIL_;

    hr = f(riid, out);

    /* 关键：用调用者自己的 riid 建出来的工厂去枚举适配器，不猜 IID */
    if (hr >= 0 && out && *out) patch_from_factory(*out);
    return hr;
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
    i32          hr;

    wb_init();
    if (!g_real) return E_FAIL_;

    f = (PFN_Factory2)GetProcAddress(g_real, "CreateDXGIFactory2");
    if (!f) return E_FAIL_;

    hr = f(flags, riid, out);
    if (hr >= 0 && out && *out) patch_from_factory(*out);
    return hr;
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
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH_) g_self = mod;
    return TRUE_;
}
