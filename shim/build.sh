#!/usr/bin/env bash
# 编译 wb-vram-shim（Vulkan 层，纯 C）与验证工具
#
#   ./shim/build.sh            # 编译两者
#   ./shim/build.sh clean      # 清理
#
# 依赖：gcc、dl、pthread。Vulkan 头文件已 vendor 在 third_party/vulkan/。
# 本层 **不链接 libvulkan**（层只从 loader 拿函数指针），验证工具才需要 -lvulkan。

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INC="$(cd "$HERE/../third_party" && pwd)"

CC="${CC:-gcc}"
SO="$HERE/libwb_vram_shim.so"
PROBE="$HERE/wb_vram_probe"

if [ "${1:-}" = "clean" ]; then
    rm -f "$SO" "$PROBE"
    echo "已清理"
    exit 0
fi

echo "==> 编译 Vulkan 层: libwb_vram_shim.so"
# -fvisibility=hidden + 源码里显式 WB_EXPORT，只暴露 vkGetInstanceProcAddr/vkGetDeviceProcAddr
"$CC" -O2 -g -Wall -Wextra -fPIC -fvisibility=hidden \
    -I"$INC" \
    -shared -o "$SO" "$HERE/wb_vram_shim.c" \
    -ldl -lpthread

echo "==> 编译验证工具: wb_vram_probe"
"$CC" -O2 -g -Wall -Wextra -I"$INC" \
    -o "$PROBE" "$HERE/wb_vram_probe.c" -lvulkan

echo
ls -l "$SO" "$PROBE"
echo "构建完成。下一步： ./shim/install.sh"
