#!/usr/bin/env bash
# 编译「Wine 部件版」dxgi.dll（PE32+ x86_64）
#
# 工具链全部来自本机，不需要 root、不需要下载：
#   clang --target=x86_64-pc-windows-gnu   产出 COFF 目标文件
#   ld -m i386pep                          产出 PE32+（本机 binutils 支持 pei-x86-64）
#   /usr/lib/wine/x86_64-windows/lib*.a    Wine 自带的导入库（kernel32 / advapi32）
#
# 源码刻意不 include 任何头文件，所以不需要 mingw 的 libc/头文件。
#
# 用法: ./winepart/build.sh [clean]

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/wb_vram_dxgi.c"
DEF="$HERE/dxgi.def"
OBJ="$HERE/wb_vram_dxgi.o"
OUT="$HERE/dxgi.dll"

WINDIR="${WINDIR_LIB:-/usr/lib/wine/x86_64-windows}"

if [ "${1:-}" = "clean" ]; then
    rm -f "$OBJ" "$OUT"
    echo "已清理"
    exit 0
fi

command -v clang >/dev/null || { echo "缺 clang"; exit 1; }
[ -d "$WINDIR" ] || { echo "找不到 Wine 的 Windows 库目录: $WINDIR"; exit 1; }

echo "==> 1/2 编译目标文件 (clang, COFF x86_64)"
clang --target=x86_64-pc-windows-gnu \
      -O2 -Wall -Wextra \
      -ffreestanding -fno-builtin -fno-builtin-memcpy -fno-builtin-memset \
      -nostdinc -fno-stack-protector -fno-asynchronous-unwind-tables \
      -c "$SRC" -o "$OBJ"

echo "==> 2/2 链接 (ld -m i386pep -> PE32+ DLL)"
ld -m i386pep -shared \
   --enable-stdcall-fixup \
   -e DllMain \
   -o "$OUT" "$OBJ" "$DEF" \
   -L"$WINDIR" -lkernel32 -ladvapi32

echo
file "$OUT"
echo
echo "产物: $OUT"
echo "安装: ./winepart/install.sh        （装进 Proton，游戏目录不动）"
