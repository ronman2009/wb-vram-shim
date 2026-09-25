#!/usr/bin/env bash
# 阶段 1：主机基线采集（不需要安装任何新软件）
#
# 用法:
#   ./scripts/00-host-baseline.sh                 # 默认标签 idle
#   ./scripts/00-host-baseline.sh idle            # 桌面空闲时
#   ./scripts/00-host-baseline.sh gta-running     # 游戏跑起来、进入战局几分钟后
#
# 输出: results/<时间戳>-host-<标签>.txt   （我会直接读这个文件）

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LABEL="${1:-idle}"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$ROOT/results/${STAMP}-host-${LABEL}.txt"

mkdir -p "$ROOT/results"
exec > >(tee "$OUT") 2>&1

hr() { printf '\n===== %s =====\n' "$*"; }
cmd() { printf '\n$ %s\n' "$*"; "$@" 2>&1 || printf '(退出码 %s)\n' "$?"; }

printf '采集时间: %s\n标签: %s\n主机: %s\n' "$(date -Is)" "$LABEL" "$(uname -n)"

hr "0. 桌面会话与屏幕"
printf 'XDG_SESSION_TYPE=%s\nWAYLAND_DISPLAY=%s\nDISPLAY=%s\n' \
  "${XDG_SESSION_TYPE:-}" "${WAYLAND_DISPLAY:-}" "${DISPLAY:-}"
cmd xrandr --current
for f in /sys/class/drm/card*/modes; do
  [ -r "$f" ] && { printf '\n%s 的首个模式: ' "$f"; sed -n '1p' "$f"; }
done

hr "1. nvidia-smi 显存总览"
cmd nvidia-smi
cmd nvidia-smi --query-gpu=index,name,driver_version,memory.total,memory.used,memory.free --format=csv

hr "2. nvidia-smi 显存/BAR1 明细（重点看 BAR1 与 ReBAR）"
cmd nvidia-smi -q -d MEMORY
cmd nvidia-smi -q

hr "3. 驱动与内核"
cmd cat /proc/driver/nvidia/version
for f in /proc/driver/nvidia/gpus/*/information; do
  [ -r "$f" ] && { printf '\n--- %s\n' "$f"; cat "$f"; }
done
cmd cat /proc/cmdline
cmd lsmod
cmd cat /proc/meminfo

hr "4. PCI 侧确认 Resizable BAR / BAR 大小"
cmd lspci -vv -s 05:00.0

hr "5. 原生 Vulkan：适配器与堆大小"
cmd vulkaninfo --summary

hr "6. 原生 Vulkan：memoryHeaps 与 memoryBudget（VK_EXT_memory_budget）"
printf '\n-- 抓取 heap 相关段落 --\n'
vulkaninfo 2>/dev/null | grep -n -i -E 'heapBudget|heapUsage|memoryHeap|heapFlags|size *=|device-local|DEVICE_LOCAL|VK_EXT_memory_budget|pageable' \
  | sed -n '1,200p'

hr "7. 当前谁是显存大户"
cmd nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv

hr "8. 收尾"
printf '\n完整输出已写入: %s\n' "$OUT"
