#!/usr/bin/env bash
# ============================================================================
# winepart/install-v2.sh —— 专门装 v2（当前 winepart/dxgi.dll）的一键入口
#
# 为什么单独给一个入口：
#   winepart/install.sh         通用安装器（配 --dll 可装任意一份）
#   winepart/v1fix/install.sh   v1 对照组
#   本脚本固定装 v2，并在安装前后把该看的信息打全，省得记参数。
#   它自己不实现任何安装逻辑，全部委托给上面两个脚本。
#
# 用法:
#   ./winepart/install-v2.sh               装 v2
#   ./winepart/install-v2.sh --status      只看现在装的是什么（只读）
#   ./winepart/install-v2.sh --dry-run     只打印将要执行的命令，不动手
#   ./winepart/install-v2.sh --no-nvml     不装 NVML 垫片（默认会装，算法需要它）
#   ./winepart/install-v2.sh uninstall     还原成官方原件
#   ./winepart/install-v2.sh rollback      急救：恢复官方 dxgi.dll + 清理所有痕迹
#
# 其它可透传的选项（同 install.sh）:
#   --appid N   目标 appid，默认 3240220（GTA V Enhanced）
#   --proton D  手动指定 Proton 目录    --prefix D  手动指定 pfx 目录
# ============================================================================
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OURS="$HERE/dxgi.dll"
INSTALL="$HERE/install.sh"
ROLLBACK="$HERE/rollback.sh"

hr() { printf '%s\n' "------------------------------------------------------------"; }

# 靠**内容标记**识别版本（不看哈希 —— 哈希判身份曾经害我们丢过官方原件）
ver_of() {
    local f="$1"
    [ -f "$f" ] || { echo "文件不存在"; return; }
    if strings -el "$f" 2>/dev/null | grep -q 'wbshim-v32'; then
        echo "v3.2（默认全游戏生效）"
    elif strings -el "$f" 2>/dev/null | grep -q 'wbshim-v31'; then
        echo "v3.1（逐进程枚举，带白名单）"
    elif strings -el "$f" 2>/dev/null | grep -q 'wbshim-v3-wholecard'; then
        echo "v3.0（整卡减法，已废弃）"
    elif strings -a "$f" 2>/dev/null | grep -q 'GraphicsRunningProcesses_v2'; then
        echo "v3.0-旧（枚举进程，已废弃）"
    elif strings -el "$f" 2>/dev/null | grep -q '^WBVRAM_AUTO$'; then
        echo "v2.2–v2.4（路线 B，已废弃）"
    elif strings -el "$f" 2>/dev/null | grep -q '^WBVRAM_DYNAMIC$'; then
        echo "v2.0–v2.1（没有路线 B）"
    elif grep -qa 'wb-vram-shim' "$f" 2>/dev/null; then
        echo "v1 系列（多半是 v1fix 对照组）"
    else
        echo "不是我们的组件（可能是官方 DXVK 原件）"
    fi
}

# ---- 只读 / 急救类动作：直接转交，不做任何前置处理 -------------------------
case "${1:-}" in
    --status|status)     exec "$INSTALL" status ;;
    rollback|--rollback) shift; exec "$ROLLBACK" "$@" ;;
    uninstall)           shift; exec "$INSTALL" uninstall "$@" ;;
    -h|--help)           sed -n '2,20p' "$0"; exit 0 ;;
esac

# ---- 以下是安装路径：收集要透传给 install.sh 的参数 ------------------------
DRY=0
PASS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run)                                DRY=1; shift ;;
        --appid|--proton|--prefix)                PASS+=("$1" "$2"); shift 2 ;;
        --with-nvml|--no-nvml|--no-prefix-copy)   PASS+=("$1"); shift ;;
        *) echo "未知参数: $1"; echo "看帮助: $0 --help"; exit 1 ;;
    esac
done

# ---- 前置检查 --------------------------------------------------------------
[ -x "$INSTALL" ] || { echo "找不到 $INSTALL（仓库不完整？）"; exit 1; }
if [ ! -f "$OURS" ]; then
    echo "没找到 $OURS —— 先编译：./winepart/build.sh"
    exit 1
fi

echo "要安装的组件 : $OURS"
echo "版本识别     : $(ver_of "$OURS")"
printf '大小 / 指纹  : %s 字节  %s…\n' "$(stat -c%s "$OURS")" "$(sha256sum "$OURS" | cut -c1-16)"
hr
echo "安装前状态："
"$INSTALL" status ${PASS[@]+"${PASS[@]}"} 2>/dev/null || true
hr

if [ "$DRY" = 1 ]; then
    echo "[dry-run] 将要执行："
    echo "  $INSTALL install ${PASS[*]:-}"
    echo "（什么都没动）"
    exit 0
fi

echo "==> 开始安装"
"$INSTALL" install "${PASS[@]}"
rc=$?
hr

if [ "$rc" != 0 ]; then
    echo "安装没成功（退出码 $rc）。"
    echo "急救：./winepart/install-v2.sh rollback"
    exit "$rc"
fi

cat <<'TIPS'
装完了。**不用改任何启动项** —— 直接启动游戏就生效。

1) 启动一次游戏，确认能正常进、不崩。
   日志（宿主路径）：
     <pfx>/drive_c/users/steamuser/AppData/Local/Temp/wbvram.log
   里面应该能看到这几行：
     [结果] total=8192 MiB ; 算法=NVML 枚举各进程占用 → total − 其它 − margin => budget=...
     [NVML] 枚举进程=N 个（其中无用量=0）除本进程外合计=X MiB ; margin=200 => budget=Y MiB
     探测通过：DedicatedVideoMemory=8160 MiB, Budget=..., Usage=...
     已挂钩适配器虚表: GetDesc=... QueryVideoMemoryInfo=... GetDesc3=挂钩

2) 想核对它算得对不对：把日志里的「除本进程外合计=X」跟 nvtop 里非游戏进程的合计对一下。
   对得上 -> 一切正常，什么都不用做。

3) 游戏内也能直接看：设置->图形，分母应该是 8192，
   「其它软件」那一行 = 8192 − 报出值。

算法（默认，装完即生效）：
   others = 所有进程的显存占用之和，**剔除本进程自己**
   budget = 物理总量 8192 − others − 200 MiB
  认自己：拿自己的 exe 名去读 NVML 列表里每个进程的 /proc/<pid>/cmdline 找同名
           （Wine 的 /proc/self 指向 wineserver，不能用来认自己 —— 实测确认）
   每 500ms 重算一次 —— 桌面占用变化会实时反映到额度上。

想调（都不用重装，加到启动项 %command% 前面即可）：
     WBVRAM_MARGIN_MB=N   改安全垫（默认 200，越小给游戏越多、也越贴边）
     WBVRAM_DYNAMIC=0     退回固定余量（8192 − WBVRAM_RESERVE_MB，默认 1536）
     WBVRAM_DRYRUN=1      只观测不改写

日常操作：
  ./winepart/install-v2.sh --status     看现在装的是哪一版
  ./winepart/install-v2.sh uninstall    还原成官方原件
  ./winepart/install-v2.sh rollback     出问题时的急救（含清理所有前缀痕迹）

记得：Steam「验证 Proton 文件完整性」或 Proton 自动更新会把 dxgi.dll 换回官方版，
status 一查就知道，重跑本脚本即可。

TIPS
