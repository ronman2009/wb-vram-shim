#!/usr/bin/env bash
# ============================================================================
# 一键回退：把 Proton 里的 dxgi.dll 恢复成官方原件，并清掉我们添加的所有文件
#
#   ./winepart/rollback.sh                执行
#   ./winepart/rollback.sh --dry-run      只看会做什么，不落笔
#   ./winepart/rollback.sh --appid N      指定 appid（默认 3240220）
#   ./winepart/rollback.sh --proton DIR   指定 Proton（默认从前缀软链反查）
#
# 它做四件事：
#   1) 找一个**不是我们**的 dxgi.dll 当恢复源；优先挑与官方 md5 逐字节一致的那份；
#   2) 用它覆盖 <Proton>/files/lib/wine/dxvk/x86_64-windows/dxgi.dll；
#   3) 删掉我们加的文件（原件备份、前缀副本、NVML 垫片、标记文件）；
#   4) 扫一遍所有 compatdata 前缀，把里面我们留下的痕迹也清掉。
#
# 为什么要扫全部前缀：Proton 每次启动都会把 Proton 目录里的 dkvk dll 复制进**该游戏**
# 的前缀。所以 Proton 目录一旦被我们改过，凡是这轮启动过的游戏前缀里都可能躺着我们的
# dxgi.dll —— 只清 3240220 是不够的。
# ============================================================================

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STEAM="${STEAM_DIR:-$HOME/.local/share/Steam}"
APPID="3240220"
PROTON=""
DRY=0

# 本机 Proton Experimental 官方 dxgi.dll 的 md5（2026-09-24 首次安装前实测记录）
# 用途：优先挑出**与官方逐字节一致**的恢复源，避免误用别的 DXVK 版本。
KNOWN_GOOD_MD5="182768d50de7624b68975c88a60c1a55"

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) DRY=1; shift ;;
        --appid)   APPID="$2"; shift 2 ;;
        --appid=*) APPID="${1#*=}"; shift ;;
        --proton)  PROTON="$2"; shift 2 ;;
        --proton=*) PROTON="${1#*=}"; shift ;;
        -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
        *) echo "未知参数: $1"; exit 1 ;;
    esac
done

md5() { md5sum "$1" 2>/dev/null | cut -d' ' -f1; }

# 判断一个 DLL 是不是"我们自己造的"：我们的组件里含 UTF-16 标记串。
# 优先用 python3，其次 strings，最后保守返回"是"（宁可不动别人的文件）。
has_our_marker() {
    local f="$1"
    [ -f "$f" ] || return 1
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$f" <<'PY'
import sys
p = sys.argv[1]
try:
    d = open(p, "rb").read()
except OSError:
    sys.exit(1)
for tok in ("wb-vram-shim", "wb-vram-dropin", "wbvram_dxvk_dxgi"):
    pat = tok.encode("utf-16-le")
    if pat in d:
        sys.exit(0)
sys.exit(1)
PY
        return $?
    fi
    if command -v strings >/dev/null 2>&1; then
        strings -el "$f" 2>/dev/null | grep -qE "wb-vram-shim|wb-vram-dropin|wbvram_dxvk_dxgi"
        return $?
    fi
    echo "  （没有 python3 也没有 strings，保守起见按「是我们的文件」处理）" >&2
    return 0
}

run() {
    if [ "$DRY" = 1 ]; then printf '  [dry-run] %s\n' "$*"; else "$@"; fi
}

hr() { printf '%s\n' "------------------------------------------------------------"; }

# ---- 定位 prefix / Proton --------------------------------------------------
PFX="$STEAM/steamapps/compatdata/$APPID/pfx"
[ -d "$PFX" ] || { echo "找不到前缀: $PFX（用 --appid 指定）"; exit 1; }

if [ -z "$PROTON" ]; then
    LINK="$PFX/drive_c/windows/explorer.exe"
    if [ -L "$LINK" ]; then
        T="$(readlink -f "$LINK")"
        case "$T" in
            */files/lib/wine/*-windows/explorer.exe) PROTON="${T%%/files/lib/wine/*}" ;;
        esac
    fi
fi
[ -n "$PROTON" ] && [ -d "$PROTON" ] || { echo "找不到 Proton，用 --proton 指定"; exit 1; }

DXGI_DIR="$PROTON/files/lib/wine/dxvk/x86_64-windows"
TARGET="$DXGI_DIR/dxgi.dll"
BACKUP="$DXGI_DIR/wbvram_dxvk_dxgi.dll"
PREFIX_COPY="$PFX/drive_c/windows/system32/wbvram_dxvk_dxgi.dll"
NVML_DLL="$PROTON/files/lib/wine/x86_64-windows/nvml.dll"
NVML_SO="$PROTON/files/lib/wine/x86_64-unix/nvml.so"
NVML_MARK="$PROTON/files/lib/wine/x86_64-windows/wbvram-nvml.installed"

hr
echo "appid    : $APPID"
echo "Proton   : $PROTON"
echo "目标文件 : $TARGET"
[ "$DRY" = 1 ] && echo "模式     : DRY-RUN（不落笔）"
hr

[ -f "$TARGET" ] || { echo "目标不存在，Proton 装得不完整？"; exit 1; }

echo "==> 1/4 现状"
if has_our_marker "$TARGET"; then echo "   dxgi.dll        : 我们的（会被替换）"
else                            echo "   dxgi.dll        : 不是我们的（md5 $(md5 "$TARGET")）"; fi
if [ -f "$BACKUP" ]; then
    if has_our_marker "$BACKUP"; then echo "   原件备份        : ⚠ 已被污染（备份本身是我们的组件）"
    else                            echo "   原件备份        : 是官方原件（md5 $(md5 "$BACKUP")）"; fi
else echo "   原件备份        : 不在"; fi

echo
echo "==> 2/4 找恢复源"
# 用临时文件而不是进程替换 < <(...)：某些环境（容器、精简 /dev）里 /dev/fd 不可用
CAND_LIST="$(mktemp 2>/dev/null || echo "/tmp/wbvram-cands.$$")"
: > "$CAND_LIST"
[ -f "$BACKUP" ] && printf '%s\n' "$BACKUP" >> "$CAND_LIST"
find "$STEAM/steamapps/compatdata" -maxdepth 6 \
     -path '*/pfx/drive_c/windows/system32/dxgi.dll' 2>/dev/null | sort >> "$CAND_LIST"
find "$STEAM/steamapps/common" "$STEAM/compatibilitytools.d" -maxdepth 7 \
     -path '*dxvk/x86_64-windows/dxgi.dll' 2>/dev/null | sort >> "$CAND_LIST"

cands=()
while IFS= read -r f; do [ -n "$f" ] && cands+=("$f"); done < "$CAND_LIST"
rm -f "$CAND_LIST"

BEST=""; BEST_HOW=""; BEST_EXACT=0
for f in "${cands[@]}"; do
    [ "$f" = "$TARGET" ] && continue
    [ -f "$f" ] || continue
    if has_our_marker "$f"; then continue; fi
    m="$(md5 "$f")"
    if [ "$m" = "$KNOWN_GOOD_MD5" ]; then
        BEST="$f"; BEST_HOW="与官方逐字节一致"; BEST_EXACT=1; break
    fi
    if [ -z "$BEST" ]; then
        BEST="$f"; BEST_HOW="非我方文件（md5 $(echo "$m" | cut -c1-16)…，$(stat -c%s "$f") 字节）"
    fi
done

if [ -z "$BEST" ]; then
    hr
    echo "✘ 找不到可用的官方原件 —— 磁盘上所有候选都是我们的组件或不可用。"
    echo
    echo "请用 Steam 恢复：库 → 右键「Proton - Experimental」→ 属性 → 已安装文件 →"
    echo "验证文件完整性；或者把该游戏的兼容层临时换成 GE-Proton11-7（它的目录没被动过）。"
    echo "我已经把恢复源列出来供你自己核对："
    for f in "${cands[@]}"; do echo "   $f"; done
    exit 2
fi
echo "   恢复源 : $BEST"
echo "   依据   : $BEST_HOW"

echo
echo "==> 3/4 恢复 dxgi.dll"
run chmod u+w "$TARGET" 2>/dev/null || true
run cp -f "$BEST" "$TARGET"
run chmod 644 "$TARGET"
if [ "$DRY" = 0 ]; then
    NEW="$(md5 "$TARGET")"
    if [ "$BEST_EXACT" = 1 ] && [ "$NEW" = "$KNOWN_GOOD_MD5" ]; then
        echo "   ✔ 已恢复，md5 = $NEW（与官方原件一致）"
    elif [ "$NEW" = "$KNOWN_GOOD_MD5" ]; then
        echo "   ✔ 已恢复，md5 = $NEW（与官方原件一致）"
    else
        echo "   ⚠ 已恢复，但 md5 = $NEW ≠ 官方 $KNOWN_GOOD_MD5"
        echo "     说明用的是别的 DXVK 版本，可能与 d3d11/d3d12 不配套 —— 建议改用 Steam 验证。"
    fi
fi

echo
echo "==> 4/4 清掉我们添加的文件"
run rm -f "$BACKUP"
echo "   - 原件备份      : $BACKUP"
run rm -f "$PREFIX_COPY"
echo "   - 前缀内副本    : $PREFIX_COPY"
if [ -f "$NVML_MARK" ]; then
    run rm -f "$NVML_DLL" "$NVML_SO" "$NVML_MARK"
    echo "   - NVML 垫片     : 已移除"
fi

n_prefix=0
P_LIST="$(mktemp 2>/dev/null || echo "/tmp/wbvram-pfxs.$$")"
find "$STEAM/steamapps/compatdata" -maxdepth 5 -type d \
     -path '*/pfx/drive_c/windows/system32' 2>/dev/null | sort > "$P_LIST"
while IFS= read -r d; do
    [ -d "$d" ] || continue
    if [ -f "$d/wbvram_dxvk_dxgi.dll" ]; then
        run rm -f "$d/wbvram_dxvk_dxgi.dll"; n_prefix=$((n_prefix+1))
    fi
    if [ -f "$d/dxgi.dll" ] && has_our_marker "$d/dxgi.dll"; then
        run cp -f "$BEST" "$d/dxgi.dll"; run chmod 644 "$d/dxgi.dll"
        n_prefix=$((n_prefix+1))
    fi
done < "$P_LIST"
rm -f "$P_LIST"
echo "   - 其它前缀      : 处理了 $n_prefix 处"

echo
if [ "$DRY" = 1 ]; then
    echo "   （以下是实际要动的东西，DRY-RUN 里一律没动）"
    echo "     $BACKUP   ← 删"
    echo "     $PREFIX_COPY   ← 删"
    echo "     $TARGET   ← 从恢复源覆盖"
else
    echo "   （结果：）"
    echo "     $BACKUP   ← 已删"
    echo "     $PREFIX_COPY   ← 已删"
    echo "     $TARGET   ← 已恢复官方"
fi

hr
if [ "$DRY" = 1 ]; then
    echo "DRY-RUN 结束，什么都没改。去掉 --dry-run 真正执行。"
else
    cat <<'TIPS'
回退完成。检查一下：

  1) 文件确实是官方的：
       md5sum "<上面那个 TARGET>"
     应为 182768d50de7624b68975c88a60c1a55
  2) 游戏能正常启动（先别装任何东西）
  3) 要不要再装：从「干净状态」重来
       ./winepart/install.sh              # v2（推荐，带防嵌套自检）
       ./winepart/v1fix/install.sh        # v1 + 仅修 IID（对照组）

若 1) 的 md5 对不上，说明恢复源是别的 DXVK 版本，请改用 Steam
「验证 Proton - Experimental 文件完整性」。
TIPS
fi
