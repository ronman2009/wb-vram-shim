#!/usr/bin/env bash
# 不依赖 GPU 的自检：确认 .so 能被 dlopen、导出函数表正确、构造函数生效。
#
#   ./shim/selftest.sh
#
# 这能覆盖"层写错了"这一类问题；覆盖不到的是"有 GPU 时数值对不对"，
# 那部分必须在本机桌面环境里跑 wb_vram_probe 和游戏。

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SO="$HERE/libwb_vram_shim.so"

[ -f "$SO" ] || { echo "先编译： ./shim/build.sh"; exit 1; }

TMP="$(mktemp -d)"
cat > "$TMP/t.c" <<'CEOF'
#include <dlfcn.h>
#include <stdio.h>

typedef void *(*gipa_t)(void *, const char *);

static const char *cases[] = {
    "vkCreateInstance",
    "vkEnumeratePhysicalDevices",
    "vkGetPhysicalDeviceMemoryProperties",
    "vkGetPhysicalDeviceMemoryProperties2",
    "vkGetInstanceProcAddr",
    "vkGetDeviceProcAddr",
    "vkCmdDraw",              /* 不该拦截 */
    "vkFooBarNotAThing",      /* 不该拦截 */
    NULL
};

int main(int argc, char **argv)
{
    void  *h;
    gipa_t gipa;
    int    i, fail = 0;

    if (argc < 2) { printf("用法: %s <layer.so>\n", argv[0]); return 2; }

    h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen 失败: %s\n", dlerror()); return 1; }
    printf("dlopen 成功: %s\n", argv[1]);

    gipa = (gipa_t)dlsym(h, "vkGetInstanceProcAddr");
    if (!gipa) { printf("找不到导出符号 vkGetInstanceProcAddr\n"); return 1; }

    printf("\n入口表检查（期望：前 6 个拦截，后 2 个放行）:\n");
    for (i = 0; cases[i]; i++) {
        void *p = gipa(NULL, cases[i]);
        printf("  %-42s -> %s\n", cases[i], p ? "已拦截" : "放行(NULL)");
    }

    if (!gipa(NULL, "vkCreateInstance"))                              fail = 1;
    if (!gipa(NULL, "vkEnumeratePhysicalDevices"))                    fail = 1;
    if (!gipa(NULL, "vkGetPhysicalDeviceMemoryProperties"))           fail = 1;
    if (!gipa(NULL, "vkGetPhysicalDeviceMemoryProperties2"))          fail = 1;
    if (gipa(NULL, "vkCmdDraw"))                                      fail = 1;
    if (gipa(NULL, "vkFooBarNotAThing"))                              fail = 1;

    printf("\n%s\n", fail ? "自检失败" : "自检通过");
    return fail;
}
CEOF

gcc -O1 -Wall -Wextra -o "$TMP/t" "$TMP/t.c" -ldl
WBVRAM_LOG_FILE="$TMP/layer.log" WBVRAM_LOG=0 "$TMP/t" "$SO"

echo
echo "--- 层自己的日志（验证构造函数是否跑到）---"
if [ -f "$TMP/layer.log" ]; then cat "$TMP/layer.log"; else echo "（没有日志，构造函数可能没执行）"; fi
echo
echo "临时目录保留在: $TMP"
