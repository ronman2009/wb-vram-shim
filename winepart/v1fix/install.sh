#!/usr/bin/env bash
# 安装 v1fix 对照组（v1 原始二进制 + 仅修 IID）
#
#   ./winepart/v1fix/install.sh              # 装（复用 ../install.sh 的安装/备份/回滚逻辑）
#   ./winepart/v1fix/install.sh status       # 看状态
#   ./winepart/v1fix/install.sh uninstall    # 卸载
#
# 先决条件：磁盘上得有一份**干净的**官方 dxgi.dll 能给我们的代理转发。
# 如果刚跑过 rollback.sh 就已经满足；若提示"备份已被污染"，先跑：
#   ./winepart/rollback.sh

set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

[ -f "$HERE/dxgi.dll" ] || "$HERE/build.sh"

exec "$HERE/../install.sh" --dll "$HERE/dxgi.dll" "$@"
