#!/usr/bin/env bash
# 生成 v1fix 的 dxgi.dll（= v1 原始二进制 + 仅修 IID 的 16 字节）
#
# 对照组的意义：v1 源码已被 v2 覆盖，所以直接对 v1 的原始二进制做最小补丁，
# 这样"改动范围"本身就可证明 —— 便于判定线上问题到底是不是 v2 引入的。

set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$HERE/make-iidfix.py"
