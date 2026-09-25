#!/usr/bin/env bash
# 安装 wb-vram-shim 的层清单（manifest）
#
#   ./shim/install.sh                       # 装到 ~/.local/share/vulkan/explicit_layer.d
#   ./shim/install.sh /some/ascii/dir       # 装到别处（配合 VK_LAYER_PATH，便于测试）
#   ./shim/install.sh uninstall             # 从默认位置移除
#
# 两个刻意的设计：
#   1) 本层是 explicit layer，装了不会自动生效，必须在启动项里用
#      VK_INSTANCE_LAYERS=VK_LAYER_WB_vram_shim 打开 —— 不影响其他程序。
#   2) **会把 .so 复制到 manifest 所在目录，并用相对路径引用**。
#      原因：Vulkan loader 不认 library_path 里的非 ASCII 字符
#      （本项目路径含中文 "proton显存问题探究"，实测会被静默忽略），
#      而 ~/.local/share/vulkan/explicit_layer.d 是纯 ASCII 的。

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SO="$HERE/libwb_vram_shim.so"
TPL="$HERE/wb_vram_shim.json.in"
DEFAULT_DIR="$HOME/.local/share/vulkan/explicit_layer.d"
NAME="VK_LAYER_WB_vram_shim.json"
SONAME="libwb_vram_shim.so"

if [ "${1:-}" = "uninstall" ]; then
    removed=0
    for f in "$DEFAULT_DIR/$NAME" "$DEFAULT_DIR/$SONAME"; do
        if [ -f "$f" ]; then rm -f "$f"; echo "已移除 $f"; removed=1; fi
    done
    [ "$removed" = 1 ] || echo "默认位置没有装过"
    exit 0
fi

DIR="${1:-$DEFAULT_DIR}"

[ -f "$SO" ]  || { echo "先编译： ./shim/build.sh"; exit 1; }
[ -f "$TPL" ] || { echo "缺少模板 $TPL"; exit 1; }

case "$DIR" in
    *[!\ -~]*) echo "警告：目标目录含非 ASCII 字符，loader 可能忽略该层：$DIR" ;;
esac

mkdir -p "$DIR"
cp -f "$SO" "$DIR/$SONAME"
sed "s|__LIB_PATH__|$SONAME|" "$TPL" > "$DIR/$NAME"

echo "==> 已安装:"
echo "    $DIR/$NAME"
echo "    $DIR/$SONAME"
echo
echo "---------------------------------------------------------------"
echo "1) 先原生验证（不用开游戏，直接对比两组数字）："
echo
echo "   $HERE/wb_vram_probe"
echo "   VK_INSTANCE_LAYERS=VK_LAYER_WB_vram_shim WBVRAM_LOG=1 $HERE/wb_vram_probe"
echo
echo "2) 再在 Wine / Proton 里启用（游戏启动项，放在 %command% 前面）："
echo
echo "   VK_INSTANCE_LAYERS=VK_LAYER_WB_vram_shim WBVRAM_LOG=1 %command%"
echo
echo "   想只记录不改写、先看它算出来的数对不对："
echo "   VK_INSTANCE_LAYERS=VK_LAYER_WB_vram_shim WBVRAM_DRYRUN=1 WBVRAM_LOG=1 %command%"
echo "---------------------------------------------------------------"
echo
echo "日志 ： ~/.cache/wb-vram-shim.log"
echo "关闭 ： 去掉启动项里的 VK_INSTANCE_LAYERS；或 ./shim/install.sh uninstall"
