#!/usr/bin/env bash
# 阶段 2：在独立的 Proton 兼容容器里运行任意 Windows 程序
#
# 这个脚本不会碰游戏（compatdata/3240220），它用自己独立的前缀目录
#   prefixes/scratch/pfx
# 首次运行会初始化前缀（wineboot，约 1~2 分钟），并自动装入 GE-Proton 自带的
# DXVK / vkd3d-proton / dxvk-nvapi。
#
# 用法:
#   ./scripts/10-proton-container.sh probe/dxgi_vram_probe.exe
#   ./scripts/10-proton-container.sh "C:/windows/system32/dxdiag.exe"
#   PROTON=/path/to/other/proton ./scripts/10-proton-container.sh some.exe
#
# 输出: results/<时间戳>-proton-<程序名>.txt

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROTON="${PROTON:-$HOME/.local/share/Steam/compatibilitytools.d/GE-Proton11-7-x86_64}"
PREFIX="$ROOT/prefixes/scratch"
EXE="${1:?用法: $0 <要运行的可执行文件路径> [额外参数...]}"
shift || true

EXE_ABS="$(readlink -f "$EXE")"
NAME="$(basename "$EXE_ABS" .exe)"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$ROOT/results/${STAMP}-proton-${NAME}.txt"

mkdir -p "$PREFIX" "$ROOT/results" "$(dirname "$EXE_ABS")"

[ -x "$PROTON/proton" ] || { echo "找不到 proton: $PROTON/proton" >&2; exit 1; }

export STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.local/share/Steam"
export STEAM_COMPAT_DATA_PATH="$PREFIX"
export STEAM_COMPAT_INSTALL_PATH="$(dirname "$EXE_ABS")"
export STEAM_COMPAT_APP_ID=0

# ---- 显存诊断开关（首次排查可全开，稳定后按需减少） --------------------
# vkd3d-proton: log_memory_budget 会打印显存预算
export VKD3D_CONFIG="${VKD3D_CONFIG:-log_memory_budget,no_upload_hvv}"
export VKD3D_DEBUG="${VKD3D_DEBUG:-warn}"
# DXVK: debug 级别会在工作目录写 dxvk.log
export DXVK_LOG_LEVEL="${DXVK_LOG_LEVEL:-debug}"
# 需要时再开：图形化 HUD（探测器没有窗口，HUD 看不到，留给游戏用）
# export DXVK_HUD=devinfo,memory

# ---- 同时记录外部显存占用，便于交叉验证 --------------------------------
POLL="$ROOT/results/${STAMP}-proton-${NAME}-nvidia-smi.csv"
( for _ in $(seq 1 120); do
    nvidia-smi --query-gpu=timestamp,memory.total,memory.used,memory.free \
      --format=csv,noheader >>"$POLL" 2>/dev/null
    sleep 1
  done ) &
POLL_PID=$!

cd "$(dirname "$EXE_ABS")"

{
  printf '######## Proton 容器显存探测 ########\n'
  printf '时间: %s\n程序: %s\nProton: %s\n前缀: %s\n' \
    "$(date -Is)" "$EXE_ABS" "$PROTON" "$PREFIX"
  printf 'VKD3D_CONFIG=%s\nVKD3D_DEBUG=%s\nDXVK_LOG_LEVEL=%s\n' \
    "$VKD3D_CONFIG" "$VKD3D_DEBUG" "$DXVK_LOG_LEVEL"
  printf '######## 程序输出 ########\n'
  python3 "$PROTON/proton" run "$EXE_ABS" "$@"
  printf '\n######## 退出码 %s ########\n' "$?"
} 2>&1 | tee "$OUT"

kill "$POLL_PID" 2>/dev/null

printf '\n完整输出: %s\n' "$OUT"
[ -s "$POLL" ] && printf '显存采样: %s\n' "$POLL"
printf '提示：程序工作目录下的 dxvk.log / *.log 也请保留，我会一起看。\n'
