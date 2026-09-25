#!/usr/bin/env bash
# =============================================================================
# 20-vram-recon.sh —— 只读侦察：算清"这块卡还有多少空间可以榨"
#
# 用法：游戏跑起来、进到最吃显存的场景后，在另一个终端跑
#         ./scripts/20-vram-recon.sh
#
# 纯只读：不碰任何文件、不改注册表、不碰 Proton。
# 依赖：nvidia-smi（你机器上就有）。沙箱里没 /dev/nvidia*，必须在你的终端跑。
# =============================================================================
set -u

PHYS_MIB=8192          # 物理总量
CUR_FLOOR=6656         # 当前地板价 = 8192 - reserve(1536)
CUR_RESERVE=1536

hr() { printf '%s\n' "----------------------------------------------------------------"; }
kv() { printf '  %-26s %s\n' "$1" "$2"; }

command -v nvidia-smi >/dev/null || { echo "找不到 nvidia-smi"; exit 1; }

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK" 2>/dev/null' EXIT

hr
echo "GPU 总览"
hr
nvidia-smi --query-gpu=name,memory.total,memory.used,memory.free,utilization.gpu \
           --format=csv,noheader 2>/dev/null | sed 's/^/  /'

# ---- 进程级占用：图形进程与计算进程是两个列表，都要取 --------------------
RAW="$WORK/raw.csv"
{
  nvidia-smi --query-graphics-apps=pid,process_name,used_memory \
             --format=csv,noheader,nounits 2>/dev/null
  nvidia-smi --query-compute-apps=pid,process_name,used_memory \
             --format=csv,noheader,nounits 2>/dev/null
} | sed '/^[[:space:]]*$/d' | sort -t, -k1,1n -u > "$RAW"

hr
echo "进程级占用（MiB，降序）"
hr
if [ -s "$RAW" ]; then
    awk -F, '{
        name=$2; mib=$3
        gsub(/^[ \t]+|[ \t]+$/, "", name)
        gsub(/^[ \t]+|[ \t]+$/, "", mib)
        n = split(name, seg, /[\\\/]/)
        printf "  %-46s %8s\n", substr(seg[n], 1, 46), mib
    }' "$RAW" | sort -k2,2nr
else
    echo "  （驱动没返回进程级数据。用 nvtop 看，或按下面的总量估算）"
fi

# ---- 汇总 ----------------------------------------------------------------
SUM="$WORK/sum.txt"
python3 - "$RAW" > "$SUM" 2>/dev/null <<'PY'
import sys, collections
agg = collections.OrderedDict()
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    p = [x.strip() for x in line.split(",")]
    if len(p) < 3:
        continue
    try:
        mib = int(p[2])
    except ValueError:
        continue
    key = p[1].replace("\\", "/").rsplit("/", 1)[-1]
    agg[key] = agg.get(key, 0) + mib
game = [k for k in agg if "gta" in k.lower()]
gm = sum(agg[k] for k in game)
om = sum(v for k, v in agg.items() if k not in game)
print(gm, om, (", ".join(game) if game else "(未匹配到游戏进程)"))
PY

GM=0; OM=0; GNAME="(解析失败)"
if [ -s "$SUM" ]; then
    read -r GM OM GNAME < "$SUM" 2>/dev/null || true
fi
GM="${GM:-0}"; OM="${OM:-0}"

if [ "$GM" = "0" ]; then
    USED="$(nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits 2>/dev/null | tr -d ' ')"
    GNAME="(没抓到 GPU 进程列表 —— 用 nvtop 上的数字手动填下面的算式)"
    echo
    echo "  提示：GPU 总用量 = ${USED:-?} MiB。把你 nvtop 里游戏那一行的 MiB 填到 GM 再跑。"
    OM="${USED:-0}"
fi

TOTAL=$(( GM + OM ))
FREE=$(( PHYS_MIB - TOTAL ))
LIMIT=$(( PHYS_MIB - OM ))

hr
echo "账目"
hr
kv "物理总量"              "$PHYS_MIB MiB"
kv "游戏进程"              "$GM MiB   $GNAME"
kv "其它合计"              "$OM MiB"
kv "合计占用"              "$TOTAL MiB"
kv "还剩空闲"              "$FREE MiB"
echo
kv "物理极限（其它不再涨）"  "$LIMIT MiB"
kv "当前地板价"            "$CUR_FLOOR MiB   (reserve=$CUR_RESERVE)"
echo "  → 从地板价到物理极限还有 $(( LIMIT - CUR_FLOOR )) MiB 可抬"

hr
echo "reserve 三档（地板价 = $PHYS_MIB − reserve）"
hr
for r in 1400 1280 1152; do
    f=$(( PHYS_MIB - r ))
    over=$(( LIMIT - f ))
    case $r in
        1400) tag="保守 —— 给其它进程留约 330 MiB 波动空间" ;;
        1280) tag="均衡 —— 留约 210 MiB" ;;
        1152) tag="激进 —— 余量很薄，只适合桌面干净时用" ;;
    esac
    printf '  reserve=%-5s -> 地板 %-5s   距物理极限 %+5s MiB   %s\n' "$r" "$f" "$over" "$tag"
done
echo
echo "  ⚠ reserve 必须大于「其它进程真实占用」，否则游戏会转用共享显存（走 PCIe），"
echo "    表现是掉帧/卡顿/贴图延迟 —— 比显存不够更难受。"
echo "    当前其它占用 = $OM MiB，所以 reserve 不应低于 $(( OM + 100 ))。"
hr