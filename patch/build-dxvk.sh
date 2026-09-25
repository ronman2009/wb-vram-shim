#!/usr/bin/env bash
# 编译打过「LOCAL 预算下限」补丁的 DXVK，并把 dxgi.dll 装进 Proton。
#
# 用法:
#   ./patch/build-dxvk.sh                # 默认按 v3.1 标签构建（与 Proton 自带版本对齐）
#   DXVK_REF=v3.1.1 ./patch/build-dxvk.sh
#
# 依赖（Manjaro）:
#   sudo pacman -S --needed git meson ninja mingw-w64-gcc glslang
#
# 说明：必须替换 **Proton 目录里** 的 dxgi.dll，而不是前缀里的 ——
#       Proton 每次启动都会把自带的 DXVK 重新摊进 prefix，手改 prefix 会被覆盖。

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SRC="${DXVK_SRC:-$HOME/src/dxvk}"
REF="${DXVK_REF:-v3.1}"

PROTONS=(
  "$HOME/.local/share/Steam/compatibilitytools.d/GE-Proton11-7-x86_64"
  "$HOME/.local/share/Steam/steamapps/common/Proton - Experimental"
)

echo "==> 0/5 检查依赖"
missing=0
for t in git meson ninja x86_64-w64-mingw32-gcc python3; do
  command -v "$t" >/dev/null 2>&1 && printf '  ok   %s\n' "$t" || { printf '  缺   %s\n' "$t"; missing=1; }
done
[ "$missing" = 0 ] || {
  echo
  echo "先装： sudo pacman -S --needed git meson ninja mingw-w64-gcc glslang"
  exit 1
}
command -v glslangValidator >/dev/null 2>&1 || echo "  warn 没有 glslangValidator，DXVK 内部着色器可能编不过（sudo pacman -S glslang）"

echo
echo "==> 1/5 准备 DXVK 源码 ($REF) -> $SRC"
if [ ! -d "$SRC/.git" ]; then
  git clone --recursive --depth 1 --branch "$REF" https://github.com/doitsujin/dxvk "$SRC"
else
  echo "  已存在，跳过 clone"
fi

echo
echo "==> 2/5 打补丁"
python3 "$HERE/apply-budget-patch.py" "$SRC"

echo
echo "==> 3/5 交叉编译 (mingw-w64 x86_64)"
cd "$SRC"
rm -rf build
meson setup --cross-file build-win64.txt --buildtype release build
ninja -C build

DLL="$SRC/build/src/dxgi/dxgi.dll"
[ -f "$DLL" ] || { echo "没编出 dxgi.dll，检查上面的编译输出"; exit 1; }
ls -l "$DLL"

echo
echo "==> 4/5 安装到 Proton（原文件备份为 *.orig）"
for P in "${PROTONS[@]}"; do
  D="$P/files/lib/wine/dxvk/x86_64-windows/dxgi.dll"
  if [ ! -f "$D" ]; then
    echo "  跳过（不存在）: $D"
    continue
  fi
  [ -f "$D.orig" ] || cp -a "$D" "$D.orig"
  cp -f "$DLL" "$D"
  echo "  已替换: $D"
done

echo
echo "==> 5/5 完成"
cat <<'EOF'
启动项里加环境变量（放在 %command% 前面）：

    DXGI_LOCAL_BUDGET_MB=6400 %command%

调参建议（这是经验旋钮，不是越大越好）：
    先 6400  ->  每次 +256  ->  盯 DXVK 日志里的 OUT_OF_DEVICE_MEMORY / 卡顿 / 贴图丢失
    天花板就是"8160 - 其它进程真实占用"，本机实测其它占用约 1.4 GB，
    所以实用上限大约 6600~6900；直接给 8160 会把"游戏自己缩池"变成"分配失败"。

回滚：
    for f in $(find ~/.local/share/Steam -name 'dxgi.dll.orig'); do mv "$f" "${f%.orig}"; done
EOF
