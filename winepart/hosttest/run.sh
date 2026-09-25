#!/usr/bin/env bash
# 宿主端（Linux）测试：把 wb_vram_dxgi.c 直接编成 Linux 程序跑
#
#   ./winepart/hosttest/run.sh
#
# 关键编译开关：
#   -fshort-wchar        让 L"..." 是 2 字节，与源码里的 WCHAR=u16 对齐
#   -D__declspec(x)=     抹掉 dllimport/dllexport
#   -D__stdcall=         抹掉调用约定
# 被测源码不 include 任何头文件，所以能这样直接 include。

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/harness"

CC="${CC:-cc}"

echo "==> 编译测试台"
"$CC" -std=c11 -O1 -Wall -Wextra -Wno-unused-function \
      -fshort-wchar \
      -D'__declspec(x)=' -D'__stdcall=' \
      "$HERE/harness.c" -o "$OUT"

SCENES="patch nowhitelist noappid dryrun badriid reserve dynamic default noself noprocs nvml-missing toolpath nested all-ours"

pass=0; fail=0
for s in $SCENES; do
    echo
    echo "========== $s =========="
    if "$OUT" "$s"; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
    fi
done

echo
echo "============================================================"
echo "场景: 通过 $pass / $((pass+fail))"
rm -f "$OUT"
[ "$fail" = 0 ]
