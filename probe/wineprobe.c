/*
 * wineprobe.c —— 实测 Wine 下 /proc 的各种读法
 *
 * 目的：回答一个问题 —— 在 Wine 里，一个 PE 进程怎样才能可靠地拿到
 *       "自己的 Linux PID"（NVML 的进程枚举报的就是 Linux PID）。
 *
 * 结论将决定 wb_vram_dxgi.c 里"剔除自己"该用哪种方法。
 *
 * 纯 C、零头文件依赖（clang --target=x86_64-pc-windows-gnu 找不到 mingw sysroot）。
 * 编译见 build.sh，入口 wbmain。
 */

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef int                i32;
typedef unsigned long      DWORD;
typedef int                BOOL;
typedef void              *HANDLE;
typedef unsigned short     WCHAR;

#define STD_OUTPUT_HANDLE_        ((DWORD)-11)
#define INVALID_HANDLE_VALUE_     ((HANDLE)(long long)-1)
#define GENERIC_READ_             0x80000000u
#define GENERIC_WRITE_            0x40000000u
#define FILE_SHARE_READ_          0x00000001u
#define FILE_SHARE_WRITE_         0x00000002u
#define CREATE_ALWAYS_            2u
#define OPEN_EXISTING_            3u
#define FILE_ATTRIBUTE_NORMAL_    0x80u
#define FILE_ATTRIBUTE_DIRECTORY_ 0x10u

__declspec(dllimport) DWORD  __stdcall GetCurrentProcessId(void);
__declspec(dllimport) DWORD  __stdcall GetModuleFileNameW(void *h, WCHAR *buf, DWORD n);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD n);
__declspec(dllimport) BOOL   __stdcall WriteFile(HANDLE f, const void *b, DWORD n, DWORD *w, void *ov);
__declspec(dllimport) HANDLE __stdcall CreateFileW(const WCHAR *n, DWORD acc, DWORD sh,
                                                   void *sa, DWORD disp, DWORD fl, HANDLE t);
__declspec(dllimport) BOOL   __stdcall ReadFile(HANDLE f, void *b, DWORD n, DWORD *r, void *ov);
__declspec(dllimport) BOOL   __stdcall CloseHandle(HANDLE h);
__declspec(dllimport) HANDLE __stdcall FindFirstFileW(const WCHAR *pat, void *fd);
__declspec(dllimport) BOOL   __stdcall FindNextFileW(HANDLE h, void *fd);
__declspec(dllimport) BOOL   __stdcall FindClose(HANDLE h);
__declspec(dllimport) void   __stdcall Sleep(DWORD ms);

/* WIN32_FIND_DATAW 的精确布局：FILETIME 是两个 DWORD（4 字节对齐），
 * 所以 attr(4) + ft*3(24) + 4*DWORD(16) + name(520) + alt(28) = 592，无 padding。 */
typedef struct { DWORD lo, hi; } FILETIME_;
typedef struct {
    DWORD     attr;
    FILETIME_ ct, at, wt;
    DWORD     size_hi, size_lo, res0, res1;
    WCHAR     name[260];
    WCHAR     alt[14];
} FDW_;

/* ======================= 输出 ======================= */

static char  g_buf[65536];
static u32   g_len = 0;
static HANDLE g_file = INVALID_HANDLE_VALUE_;

static u32 slen(const char *s) { u32 n = 0; while (s && s[n]) n++; return n; }

static void emit(const char *s)
{
    u32 n = slen(s);
    DWORD w = 0;
    HANDLE o = GetStdHandle(STD_OUTPUT_HANDLE_);
    if (o != INVALID_HANDLE_VALUE_ && o) WriteFile(o, s, n, &w, 0);
    if (g_len + n + 1 < sizeof(g_buf)) {
        u32 i;
        for (i = 0; i < n; i++) g_buf[g_len++] = s[i];
        g_buf[g_len] = 0;
    }
}

static void emit_num(u64 v)
{
    char t[24];
    u32  n = 0;
    if (!v) { emit("0"); return; }
    while (v) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) { char c[2]; c[0] = t[--n]; c[1] = 0; emit(c); }
}

/* 宽 → ASCII（只取低字节；够用于诊断），并把 NUL 换成一个指定字符 */
static void emit_w(const WCHAR *w, u32 maxn, char nul)
{
    u32 i;
    for (i = 0; i < maxn && w[i]; i++) {
        u8 c = (u8)(w[i] & 0xFF);
        char b[2];
        if (c < 0x20) c = (u8)nul;
        b[0] = (char)c; b[1] = 0;
        emit(b);
    }
}

static void emit_bytes(const char *p, u32 n, char nul)
{
    u32 i;
    for (i = 0; i < n; i++) {
        u8 c = (u8)p[i];
        char b[2];
        if (c < 0x20) c = (u8)nul;
        b[0] = (char)c; b[1] = 0;
        emit(b);
    }
}

/* ======================= 读文件 ======================= */

/* mode: 0 = 宽路径, 1 = 直接汉字路径（\\?\unix\... 之类在 WCHAR 里给） */
static int read_into(const WCHAR *path, const char *tag, u32 keep)
{
    HANDLE f;
    char   b[4096];
    DWORD  got = 0;

    emit(tag); emit(" = ");
    f = CreateFileW(path, GENERIC_READ_, FILE_SHARE_READ_ | FILE_SHARE_WRITE_,
                    0, OPEN_EXISTING_, FILE_ATTRIBUTE_NORMAL_, 0);
    if (f == INVALID_HANDLE_VALUE_) {
        emit("[打不开]"); emit("\n");
        return 0;
    }
    if (ReadFile(f, b, sizeof(b) - 1, &got, 0) && got) {
        if (got > keep) got = keep;
        b[got] = 0;
        emit("\"");
        emit_bytes(b, got, '|');
        emit("\"\n");
    } else {
        emit("[读失败]"); emit("\n");
    }
    CloseHandle(f);
    return 1;
}

static int is_digits(const WCHAR *s)
{
    u32 i = 0;
    if (!s[0]) return 0;
    while (s[i]) { if (s[i] < '0' || s[i] > '9') return 0; i++; }
    return 1;
}

static WCHAR *w_cat(WCHAR *d, const WCHAR *s) { while (*s) *d++ = *s++; *d = 0; return d; }
static WCHAR *w_num(WCHAR *d, u64 v)
{
    WCHAR t[24]; u32 n = 0;
    if (!v) { *d++ = '0'; *d = 0; return d; }
    while (v) { t[n++] = (WCHAR)('0' + (v % 10)); v /= 10; }
    while (n) *d++ = t[--n];
    *d = 0; return d;
}

/* ======================= 主流程 ======================= */

int wbmain(void)
{
    WCHAR  path[1024];
    WCHAR *p;
    HANDLE h;
    FDW_   fd;
    DWORD  me;
    u32    shown = 0;

    g_file = CreateFileW((const WCHAR *)L"Z:\\tmp\\wineprobe.txt", GENERIC_WRITE_,
                         FILE_SHARE_READ_ | FILE_SHARE_WRITE_, 0, CREATE_ALWAYS_,
                         FILE_ATTRIBUTE_NORMAL_, 0);

    me = GetCurrentProcessId();

    emit("######## wineprobe ########\n");
    emit("wine_pid (GetCurrentProcessId) = "); emit_num(me); emit("\n");

    /* 自己的映像路径 */
    p = path; *p = 0;
    GetModuleFileNameW(0, path, 1023);
    emit("exe = "); emit_w(path, 256, ' '); emit("\n");

    emit("\n---- 1) Z:\\proc\\self\\stat ----\n");
    read_into((const WCHAR *)L"Z:\\proc\\self\\stat", "Z:self/stat", 96);

    emit("\n---- 2) \\\\?\\unix\\proc\\self\\stat （Wine 的 unix 路径前缀）----\n");
    read_into((const WCHAR *)L"\\\\?\\unix\\proc\\self\\stat", "unix:self/stat", 96);

    emit("\n---- 3) Z:\\proc\\self\\cmdline ----\n");
    read_into((const WCHAR *)L"Z:\\proc\\self\\cmdline", "Z:self/cmdline", 160);

    emit("\n---- 4) \\\\?\\unix\\proc\\self\\cmdline ----\n");
    read_into((const WCHAR *)L"\\\\?\\unix\\proc\\self\\cmdline", "unix:self/cmdline", 160);

    emit("\n---- 5) Z:\\proc\\self\\comm ----\n");
    read_into((const WCHAR *)L"Z:\\proc\\self\\comm", "Z:self/comm", 48);

    emit("\n---- 6) Z:\\proc\\self\\maps 里的前 8 行（看 Wine 怎么映射 DLL）----\n");
    read_into((const WCHAR *)L"Z:\\proc\\self\\maps", "Z:self/maps", 600);

    emit("\n---- 7) 枚举 Z:\\proc\\* 里的数字目录（前 25 个）+ 各自 cmdline / stat 首列 ----\n");
    h = FindFirstFileW((const WCHAR *)L"Z:\\proc\\*", &fd);
    if (h == INVALID_HANDLE_VALUE_) {
        emit("[FindFirstFileW 失败]\n");
    } else {
        do {
            if (!is_digits(fd.name)) continue;
            if (shown >= 25) break;
            shown++;

            emit("pid=");
            emit_w(fd.name, 12, ' ');
            emit("  cmdline=");
            {
                WCHAR *q = path;
                q = w_cat(q, (const WCHAR *)L"Z:\\proc\\");
                q = w_cat(q, fd.name);
                q = w_cat(q, (const WCHAR *)L"\\cmdline");
                read_into(path, "", 90);   /* tag 为空，直接接在后面 */
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    emit("\n---- 8) 给 Linux 侧留出抓取时间（15 秒）----\n");

    /* 落盘 */
    if (g_file != INVALID_HANDLE_VALUE_) {
        DWORD w = 0;
        WriteFile(g_file, g_buf, g_len, &w, 0);
        CloseHandle(g_file);
    }
    Sleep(15000);
    return 0;
}
