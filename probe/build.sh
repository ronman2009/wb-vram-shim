#!/usr/bin/env bash
# 交叉编译 DXGI 显存探测器为 Windows x86_64 PE
#
# 依赖（任选其一，都不在系统里，需要你先装/下载）：
#   A. mingw-w64:  sudo pacman -S mingw-w64-gcc
#   B. zig:        下载解压后  export ZIG=/path/to/zig
#
# 用法: ./probe/build.sh

set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/probe/dxgi_vram_probe.c"
OUT="$ROOT/probe/dxgi_vram_probe.exe"

if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    echo "==> 用 mingw-w64 编译"
    x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -static -o "$OUT" "$SRC" \
        -ldxgi -ld3d12 -luuid -lole32 -loleaut32
elif [ -n "${ZIG:-}" ] && [ -x "$ZIG" ]; then
    echo "==> 用 zig 编译"
    "$ZIG" cc -target x86_64-windows-gnu -O2 -static -o "$OUT" "$SRC" \
        -ldxgi -ld3d12 -luuid -lole32 -loleaut32
else
    cat >&2 <<'EOF'
找不到 Windows 交叉编译器。二选一：

  A) sudo pacman -S mingw-w64-gcc

  B) 免 root：下载 zig 单文件工具链（约 50 MB）
     https://ziglang.org/download/   解压后
     export ZIG=$PWD/zig
     再运行本脚本

装好后重新执行 ./probe/build.sh
EOF
    exit 1
fi

ls -l "$OUT"
echo "完成。接着跑: ./scripts/10-proton-container.sh probe/dxgi_vram_probe.exe"
