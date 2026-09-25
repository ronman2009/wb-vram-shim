#!/usr/bin/env bash
# ============================================================================
# 把 wb-vram 的 dxgi.dll 安装成 Proton/Wine 的「部件」——游戏目录一个字节都不动
#
#   ./winepart/install.sh                 一键安装（自动认出该 appid 实际用的 Proton）
#   ./winepart/install.sh status          看当前状态
#   ./winepart/install.sh repair          只重新同步前缀里的原件副本
#   ./winepart/install.sh uninstall       还原成官方文件
#
# 选项:
#   --appid N         目标 appid，默认 3240220（GTA V Enhanced）
#   --proton DIR      手动指定 Proton 目录
#   --prefix DIR      手动指定 pfx 目录
#   --no-prefix-copy  不往前缀 system32 放原件副本（靠 Proton 目录自愈）
#   --with-nvml       装 NVML 垫片（nvml.dll + nvml.so）—— **默认就装**，算法要用它
#   --no-nvml         不装（只在你明确想退回静态 reserve 时才用）
#   --dll PATH        指定要安装的 dxgi.dll（默认 ./winepart/dxgi.dll）
#                     v1fix 对照组：--dll ./winepart/v1fix/dxgi.dll
#
# ⚠️ 身份判定用的是**文件内容里的标记串**，不是哈希对比：
#    我们自己造的组件（v1/v2/v1fix）都含 UTF-16 串 "wb-vram-shim"，
#    官方 DXVK 的 dxgi.dll 没有。所以不会再把旧版自己误当成"官方原件"备份。
#    备份一旦登记（wbvram-orig.ok）就不会被覆盖；若发现备份本身就是我们的组件，
#    脚本会拒绝安装并让你先跑 rollback.sh。
#
# 它做的事（全部可逆）:
#   1) <Proton>/files/lib/wine/dxvk/x86_64-windows/dxgi.dll       <- 换成我们的
#   2) <Proton>/files/lib/wine/dxvk/x86_64-windows/wbvram_dxvk_dxgi.dll <- DXVK 原件备份
#   3) <pfx>/drive_c/windows/system32/wbvram_dxvk_dxgi.dll         <- 原件副本（自愈用）
#   4) 默认同时装 NVML 垫片：<Proton>/files/lib/wine/{x86_64-windows,x86_64-unix}/nvml.{dll,so}
#      （从装了的 GE-Proton 里拷一份；Wine 的 builtin 搜索路径就是 files/lib/wine）
#
# 为什么是这里：proton 脚本每次启动都会把 dxvk/*.dll 复制进前缀 system32，
# 并设 WINEDLLOVERRIDES=...;dxgi=n（只用 native）。所以这份会被 Proton 亲自
# 摆到 C:\windows\system32\dxgi.dll 由加载器载入 —— 不需要改启动项，也不会
# 在前缀里被覆盖。
# ============================================================================

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OURS="$HERE/dxgi.dll"

STEAM="${STEAM_DIR:-$HOME/.local/share/Steam}"
APPID="3240220"
PROTON=""
PFX=""
PREFIX_COPY=1
NVML=1        # 默认装：算法要读各进程的显存占用
ACTION="install"

while [ $# -gt 0 ]; do
    case "$1" in
        install|status|repair|uninstall) ACTION="$1"; shift ;;
        --appid)       APPID="$2"; shift 2 ;;
        --appid=*)     APPID="${1#*=}"; shift ;;
        --proton)      PROTON="$2"; shift 2 ;;
        --proton=*)    PROTON="${1#*=}"; shift ;;
        --prefix)      PFX="$2"; shift 2 ;;
        --prefix=*)    PFX="${1#*=}"; shift ;;
        --no-prefix-copy) PREFIX_COPY=0; shift ;;
        --with-nvml)   NVML=1; shift ;;
        --no-nvml)     NVML=0; shift ;;
        --dll)         OURS="$2"; shift 2 ;;
        --dll=*)       OURS="${1#*=}"; shift ;;
        -h|--help)     sed -n '2,34p' "$0"; exit 0 ;;
        *) echo "未知参数: $1"; exit 1 ;;
    esac
done

sha() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }
hr()  { printf '%s\n' "------------------------------------------------------------"; }

# 判断一个 PE 是不是我们自己造的：我们所有版本都含 UTF-16 标记串（官方 DXVK 没有）
has_our_marker() {
    local f="$1"
    [ -f "$f" ] || return 1
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$f" <<'PY'
import sys
try:
    d = open(sys.argv[1], "rb").read()
except OSError:
    sys.exit(1)
for tok in ("wb-vram-shim", "wb-vram-dropin", "wbvram_dxvk_dxgi"):
    if tok.encode("utf-16-le") in d:
        sys.exit(0)
sys.exit(1)
PY
        return $?
    fi
    if command -v strings >/dev/null 2>&1; then
        strings -el "$f" 2>/dev/null | grep -qE "wb-vram-shim|wb-vram-dropin|wbvram_dxvk_dxgi"
        return $?
    fi
    return 1
}

# 认出具体是哪一版 —— 光有"是我们的"标记还不够：
# v1fix 和 v2 都带 wb-vram-shim 标记，靠 WBVRAM_* 开关名来区分代次。
ver_tag() {
    local f="$1"
    [ -f "$f" ] || { echo "不存在"; return; }
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$f" <<'PY'
import sys
try:
    d = open(sys.argv[1], "rb").read()
except OSError:
    print("读不到"); sys.exit(0)
def hasw(s):
    return s.encode("utf-16-le") in d
def hasa(s):
    return s.encode("ascii") in d
if hasw("wbshim-v32"):
    print("v3.2（默认全游戏生效）")
elif hasw("wbshim-v31"):
    print("v3.1（逐进程枚举，带白名单）")
elif hasw("wbshim-v3-wholecard"):
    print("v3.0（整卡减法，已废弃）")
elif hasa("GraphicsRunningProcesses_v2"):
    print("v3.0-旧（枚举进程，已废弃）")
elif hasw("WBVRAM_AUTO"):      print("v2.2–v2.4（路线 B，已废弃）")
elif hasw("WBVRAM_DYNAMIC"):   print("v2.0–v2.1（无路线 B）")
elif hasw("wb-vram-shim"):     print("v1 系列（v1fix 对照组？）")
else:                          print("官方原件 / 第三方")
PY
        return
    fi
    if command -v strings >/dev/null 2>&1; then
        if strings -el "$f" 2>/dev/null | grep -q 'wbshim-v32'; then
            echo "v3.2（默认全游戏生效）"
        elif strings -el "$f" 2>/dev/null | grep -q 'wbshim-v31'; then
            echo "v3.1（逐进程枚举，带白名单）"
        elif strings -el "$f" 2>/dev/null | grep -q 'wbshim-v3-wholecard'; then
            echo "v3.0（整卡减法，已废弃）"

            echo "v2.2–v2.4（路线 B，已废弃）"
        elif strings -el "$f" 2>/dev/null | grep -q '^WBVRAM_DYNAMIC$'; then
            echo "v2.0–v2.1（无路线 B）"
        elif strings -el "$f" 2>/dev/null | grep -q 'wb-vram-shim'; then
            echo "v1 系列（v1fix 对照组？）"
        else
            echo "官方原件 / 第三方"
        fi
        return
    fi
    echo "无法识别（缺 python3 和 strings）"
}

# "是我们的"：内容带标记（跨版本都认）或哈希正好等于这次要装的那份
is_ours() {    local f="$1"
    [ -f "$f" ] || return 1
    has_our_marker "$f" && return 0
    [ -n "$OUR_SHA" ] && [ "$(sha "$f")" = "$OUR_SHA" ]
}

# ---- 1. 定位 prefix --------------------------------------------------------
if [ -z "$PFX" ]; then
    PFX="$STEAM/steamapps/compatdata/$APPID/pfx"
fi
if [ ! -d "$PFX" ]; then
    echo "找不到前缀: $PFX"
    echo "（appid $APPID 还没跑过？换个 --appid，或用 --prefix 指定）"
    exit 1
fi

# ---- 2. 定位这个前缀实际用的 Proton ---------------------------------------
# 前缀里的 explorer.exe 等是软链，直指当初创建它的那个 Proton —— 最可靠的证据
if [ -z "$PROTON" ]; then
    LINK="$PFX/drive_c/windows/explorer.exe"
    if [ -L "$LINK" ]; then
        TARGET="$(readlink -f "$LINK")"
        case "$TARGET" in
            */files/lib/wine/*-windows/explorer.exe)
                PROTON="${TARGET%%/files/lib/wine/*}"
                echo "从前缀软链反查到 Proton: $PROTON"
                ;;
        esac
    fi
fi
if [ -z "$PROTON" ]; then
    for c in "$STEAM/steamapps/common/Proton - Experimental" \
             "$STEAM/steamapps/common/Proton Hotfix" \
             "$STEAM/steamapps/common/Proton 9.0" ; do
        [ -d "$c/files/lib/wine/dxvk/x86_64-windows" ] && { PROTON="$c"; echo "退回到: $PROTON"; break; }
    done
fi
if [ -z "$PROTON" ] || [ ! -d "$PROTON" ]; then
    echo "找不到 Proton 目录，请用 --proton DIR 指定"
    exit 1
fi

DXGI_DIR="$PROTON/files/lib/wine/dxvk/x86_64-windows"
TARGET="$DXGI_DIR/dxgi.dll"
BACKUP="$DXGI_DIR/wbvram_dxvk_dxgi.dll"
SYSDIR="$PFX/drive_c/windows/system32"
PREFIX_COPY_DST="$SYSDIR/wbvram_dxvk_dxgi.dll"

NVML_DST_DLL="$PROTON/files/lib/wine/x86_64-windows/nvml.dll"
NVML_DST_SO="$PROTON/files/lib/wine/x86_64-unix/nvml.so"
NVML_MARK="$PROTON/files/lib/wine/x86_64-windows/wbvram-nvml.installed"

# NVML 垫片（PE + unix .so 的一对）只在部分 Proton（GE-Proton 的 nvidia-libs）里自带。
# 找一份拷过来 —— 两半必须落在同一个 WINEDLLPATH 根的 x86_64-windows / x86_64-unix 下。
find_nvml_src() {
    local roots=("$PROTON" "$STEAM/steamapps/common"/* "$STEAM/compatibilitytools.d"/*)
    local r c
    for r in "${roots[@]}"; do
        [ -d "$r" ] || continue
        for c in "$r/files/lib/wine/nvidia-libs/nvml/wine" "$r/files/lib/wine"; do
            if [ -f "$c/x86_64-windows/nvml.dll" ] && [ -f "$c/x86_64-unix/nvml.so" ]; then
                printf '%s' "$c"
                return 0
            fi
        done
    done
    return 1
}

[ -d "$DXGI_DIR" ] || { echo "不是 Proton 目录（缺 $DXGI_DIR）"; exit 1; }

OUR_SHA="$(sha "$OURS" || true)"
if [ -z "$OUR_SHA" ]; then
    echo "找不到要安装的 DLL: $OURS"
    echo "  v2 版  : ./winepart/build.sh"
    echo "  v1fix  : ./winepart/v1fix/build.sh"
    exit 1
fi

# 备份的"登记"文件：记下已验证的官方原件 md5。有它就不允许再覆盖备份。
ORIG_MARK="$DXGI_DIR/wbvram-orig.ok"

# ---- status ----------------------------------------------------------------
if [ "$ACTION" = "status" ]; then
    hr
    echo "appid         : $APPID"
    echo "prefix        : $PFX"
    echo "Proton        : $PROTON"
    echo "DXVK 版本     : $(cat "$PROTON/files/lib/wine/dxvk/version" 2>/dev/null || echo '(无 version 文件)')"
    echo "待安装的 DLL  : $OURS"
    hr
    if [ ! -f "$TARGET" ]; then
        echo "dxgi.dll      : 不存在（Proton 装得不完整？）"
    elif is_ours "$TARGET"; then
        echo "dxgi.dll      : ★ 我们的组件（已安装）｜版本: $(ver_tag "$TARGET")"
    elif [ -f "$BACKUP" ] && [ "$(sha "$TARGET")" = "$(sha "$BACKUP")" ]; then
        echo "dxgi.dll      : 官方原件（未安装）"
    else
        echo "dxgi.dll      : 不是我们的（官方原件 / Steam 更新过 / 别人替换过）"
    fi

    if [ ! -f "$BACKUP" ]; then
        echo "原件备份      : 不在（install 时会自动建）"
    elif has_our_marker "$BACKUP"; then
        echo "原件备份      : ⚠ 已被污染（备份本身是我们的组件）→ 先跑 ./winepart/rollback.sh"
    else
        echo "原件备份      : 官方原件（sha=$(sha "$BACKUP" | cut -c1-16)…）$([ -f "$ORIG_MARK" ] && echo ' [已登记]')"
    fi

    echo "前缀内副本    : $([ -f "$PREFIX_COPY_DST" ] && echo '在' || echo '不在（可 repair 补）')"
    if [ -f "$NVML_DST_DLL" ] && [ -f "$NVML_DST_SO" ]; then
        NVML_TXT="在（我们移植的那份，算法需要它）"
    elif [ -d "$PROTON/files/lib/wine/nvidia-libs/nvml/wine" ]; then
        NVML_TXT="由 Proton 自带（nvidia-libs/nvml，Proton 自己会加进 WINEDLLPATH）✔"
    else
        NVML_TXT="★ 不在 → 算法会退回静态 reserve。这个 Proton 不自带，移植也大概率不起作用，建议改用 GE-Proton"
    fi
    echo "NVML 垫片     : $NVML_TXT"
    hr
    echo "日志（宿主路径）: $PFX/drive_c/users/steamuser/AppData/Local/Temp/wbvram.log"
    exit 0
fi

# ---- 可写性检查 ------------------------------------------------------------
if [ ! -f "$TARGET" ]; then
    echo "目标不存在: $TARGET"; exit 1
fi
if ! touch "$DXGI_DIR/.wbvram-write-test" 2>/dev/null; then
    hr
    echo "没有写权限：$DXGI_DIR"
    echo "Proton 目录属主不是你。两种办法："
    echo "  sudo chown -R \"\$USER\" \"$PROTON\"        # 推荐，一劳永逸"
    echo "  sudo $0 $ACTION --proton \"$PROTON\"       # 或者整条命令用 sudo"
    hr
    exit 1
fi
rm -f "$DXGI_DIR/.wbvram-write-test"

# ---- uninstall -------------------------------------------------------------
if [ "$ACTION" = "uninstall" ]; then
    if [ -f "$BACKUP" ] && ! has_our_marker "$BACKUP"; then
        chmod u+w "$TARGET" 2>/dev/null || true
        cp -f "$BACKUP" "$TARGET"
        echo "已还原官方 dxgi.dll <- $BACKUP"
    elif [ -f "$BACKUP" ]; then
        echo "✘ 备份已被污染（备份文件本身就是我们的组件），不能拿它当官方原件还原。"
        echo "  请跑：  ./winepart/rollback.sh"
        echo "  它会从别的 compatdata 前缀里找一份与官方 md5 逐字节一致的 dxgi.dll 恢复。"
    else
        echo "没有备份文件，无法还原。"
        echo "  请跑：  ./winepart/rollback.sh"
        echo "  或用 Steam → Proton - Experimental → 属性 → 已安装文件 → 验证文件完整性。"
    fi
    if [ -f "$PREFIX_COPY_DST" ]; then
        rm -f "$PREFIX_COPY_DST" && echo "已删除前缀内副本"
    fi
    if [ -f "$NVML_MARK" ]; then
        rm -f "$NVML_DST_DLL" "$NVML_DST_SO" "$NVML_MARK"
        echo "已移除 NVML 垫片"
    fi
    rm -f "$ORIG_MARK"
    echo
    echo "卸载完成。下次启动 Proton 会自己把官方 dxgi.dll 铺回前缀。"
    exit 0
fi

# ---- install / repair ------------------------------------------------------
echo "==> 目标 Proton: $PROTON"
echo "==> 目标文件   : $TARGET"

if [ "$ACTION" = "install" ]; then
    target_ours=0; is_ours "$TARGET" && target_ours=1
    backup_ours=0; [ -f "$BACKUP" ] && has_our_marker "$BACKUP" && backup_ours=1

    # 这三种状态继续装只会制造"我们的两份 DLL 互相转发"（上次游戏起不来就是这个）
    if [ "$target_ours" = 1 ] && [ ! -f "$BACKUP" ]; then
        echo "✘ 当前的 dxgi.dll 已经是我们的组件，而且没有官方原件备份 ——"
        echo "  这份 Proton 的官方 dxgi.dll 已找不到，不能继续装。"
        echo "  先跑：  ./winepart/rollback.sh"
        exit 3
    fi
    if [ "$target_ours" = 1 ] && [ "$backup_ours" = 1 ]; then
        echo "✘ 备份已被污染（备份文件本身就是我们的组件），而当前 dxgi.dll 也是我们的。"
        echo "  先跑：  ./winepart/rollback.sh"
        exit 3
    fi

    if [ "$target_ours" = 0 ]; then
        chmod u+w "$TARGET" 2>/dev/null || true
        cp -f "$TARGET" "$BACKUP"
        chmod 644 "$BACKUP"
        echo "==> 已备份官方原件 -> $(basename "$BACKUP")  sha=$(sha "$BACKUP" | cut -c1-16)…"
        { echo "sha256=$(sha "$BACKUP")"
          echo "date=$(date -Is)"
          echo "from=$TARGET"; } > "$ORIG_MARK"
        echo "    已登记到 $(basename "$ORIG_MARK")（此后不再覆盖该备份）"
    elif [ -f "$BACKUP" ] && [ "$backup_ours" = 0 ]; then
        echo "==> 保留已登记的官方原件备份（不覆盖）"
    fi

    chmod u+w "$TARGET" 2>/dev/null || true
    cp -f "$OURS" "$TARGET"
    chmod 644 "$TARGET"
    echo "==> 已安装    : $(basename "$TARGET")  sha=$(sha "$TARGET" | cut -c1-16)…  源=$(basename "$OURS")"
fi

if [ "$ACTION" = "repair" ]; then
    if [ -f "$BACKUP" ] && has_our_marker "$BACKUP"; then
        echo "✘ 备份已被污染，repair 不安全。先跑：  ./winepart/rollback.sh"
        exit 3
    elif [ -f "$BACKUP" ]; then
        chmod u+w "$TARGET" 2>/dev/null || true
        cp -f "$OURS" "$TARGET"
        echo "==> 已重新铺上我们的 dxgi.dll（源=$(basename "$OURS")）"
    else
        echo "==> 没有原件备份，改跑 install"
        exec "$0" install --proton "$PROTON" --prefix "$PFX" --dll "$OURS" \
             $( [ "$PREFIX_COPY" = 1 ] || echo --no-prefix-copy )
    fi
fi

if [ "$PREFIX_COPY" = 1 ] && [ -f "$BACKUP" ]; then
    cp -f "$BACKUP" "$PREFIX_COPY_DST"
    echo "==> 已同步原件副本到前缀: drive_c/windows/system32/$(basename "$PREFIX_COPY_DST")"
fi

if [ "$NVML" = 1 ]; then
    # Proton 自带的 NVML（GE-Proton 的 nvidia-libs/nvml）由它自己 prepend 进 WINEDLLPATH，
    # 我们不要再往 files/lib/wine 里塞一份 —— 那只会多出冗余文件。
    if [ -d "$PROTON/files/lib/wine/nvidia-libs/nvml/wine" ]; then
        echo "==> 这个 Proton 自带 NVML 垫片（nvidia-libs/nvml）—— 无需移植，它自己会加进 WINEDLLPATH"
    else
    NVML_SRC="$(find_nvml_src || true)"
    if [ -z "$NVML_SRC" ]; then
        echo "==> 没找到 NVML 垫片来源（需要有带 nvidia-libs 的 Proton，例如 GE-Proton），跳过"
        echo "    （不影响使用：默认静态 reserve 模式不需要它）"
    else
        [ -f "$NVML_DST_DLL" ] && chmod u+w "$NVML_DST_DLL" 2>/dev/null || true
        [ -f "$NVML_DST_SO" ]  && chmod u+w "$NVML_DST_SO"  2>/dev/null || true
        mkdir -p "$(dirname "$NVML_DST_DLL")" "$(dirname "$NVML_DST_SO")"
        if [ "$NVML_SRC/x86_64-windows/nvml.dll" = "$NVML_DST_DLL" ]; then
            echo "==> NVML 垫片已在位（源即目标，无需拷贝）"
        else
            cp -f "$NVML_SRC/x86_64-windows/nvml.dll" "$NVML_DST_DLL"
            cp -f "$NVML_SRC/x86_64-unix/nvml.so"     "$NVML_DST_SO"
            echo "==> 已装 NVML 垫片 <- $NVML_SRC"
        fi
        : > "$NVML_MARK"
    fi
    fi
fi

hr
cat <<TIPS
装好了。**启动项不用改**，直接启动游戏即可。

验证：跑一次游戏后读日志（会自动追加，含所有进程）
  $PFX/drive_c/users/steamuser/AppData/Local/Temp/wbvram.log

日志里出现这几行才算真的生效：
  本模块: C:\windows\system32\DXGI.DLL
  [已加载 DXVK 原件] system32\wbvram_dxvk_dxgi.dll ...
  白名单命中 appid 3240220
  [结果] total=8192 MiB ; 算法=NVML 枚举各进程占用 → total − 其它 − margin => budget=...
  [NVML] 枚举进程=N 个（其中无用量=0）除本进程外合计=X MiB ; margin=200 => budget=Y MiB
  探测通过：DedicatedVideoMemory=8160 MiB, Budget=..., Usage=...
  已挂钩适配器虚表: GetDesc=... QueryVideoMemoryInfo=... GetDesc3=挂钩
  GetDesc: DedicatedVideoMemory 8160 -> 8192 MiB
  QueryVideoMemoryInfo(LOCAL): Budget ... -> Y MiB   (usage=... MiB)

算法（默认，装完即生效，不需要任何启动项）：
  others = 所有进程的显存占用之和，**剔除本进程自己**
  budget = 物理总量(8192) − others − 200 MiB
  认自己：拿自己的 exe 名去读 NVML 列表里每个进程的 /proc/<pid>/cmdline 找同名
           （Wine 的 /proc/self 指向 wineserver，不能用来认自己 —— 实测确认）
  每 500ms 重算一次 —— 桌面占用变化会实时反映到额度上。

调参（可选，加到启动项 %command% 前面，不用重装）：
  改安全垫：              WBVRAM_MARGIN_MB=200 %command%   （默认 200）
  退回固定余量：          WBVRAM_DYNAMIC=0 %command%        （用 WBVRAM_RESERVE_MB，默认 1536）
  只想看数据不改写：      WBVRAM_DRYRUN=1 %command%
  只对指定游戏生效：      WBVRAM_APPS=3240220,1551360 %command%   （默认全部游戏）
  临时完全关闭：          WBVRAM_ENABLE=0 %command%

⚠️ 注意
  - Steam「验证 Proton 文件完整性」或 Proton 自动更新会把 dxgi.dll 换回官方版
    （备份也可能被一起清掉）。失效就先跑 install.sh status 看一遍，再 install 一次。
  - 本文件对"用这个 Proton 的所有游戏"都可见，但 DLL 内部默认只对 appid
    3240220 生效，其它游戏一字节都不改。

出问题先跑这个，它能把 Proton 的 dxgi.dll 恢复成官方原件：
  ./winepart/rollback.sh
TIPS
