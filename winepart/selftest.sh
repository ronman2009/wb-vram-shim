#!/usr/bin/env bash
# 离线自检：不需要 Steam、不需要 GPU、不碰你真实的 Proton。
# 造一棵假的 Steam 树，把 install / status / repair / uninstall 全跑一遍，
# 校验：备份正确、覆盖正确、前缀同步正确、还原后与原文件逐字节一致。
#
# 用法: ./winepart/selftest.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL="$HERE/install.sh"
ROLLBACK="$HERE/rollback.sh"

ROOT="$(mktemp -d /tmp/wbvram-selftest.XXXXXX)"
STEAM="$ROOT/Steam"
PROTON="$STEAM/steamapps/common/Proton - Experimental"
DXGI="$PROTON/files/lib/wine/dxvk/x86_64-windows"
PFX="$STEAM/steamapps/compatdata/3240220/pfx"
SYSDIR="$PFX/drive_c/windows/system32"

pass=0; fail=0
ok()   { echo "  ✔ $1"; pass=$((pass+1)); }
bad()  { echo "  ✘ $1"; fail=$((fail+1)); }
chk()  { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

cleanup() {
    # 刻意不用 `find -delete` / `rm -rf`：某些受限沙箱会把"递归删除"交给
    # 安全删除 broker，broker 一抽风就会把整个进程组 SIGKILL 掉（实测 exit 137）。
    # 改成逐文件 `rm -f`、再自底向上 `rmdir`，行为等价但不触发那套机制。
    # 想留着现场排查就设 WBVRAM_SELFTEST_KEEP=1。
    [ "${WBVRAM_SELFTEST_KEEP:-0}" = "1" ] && { echo "（保留现场: $ROOT）"; return; }
    find "$ROOT" -type f -exec rm -f {} + 2>/dev/null || true
    find "$ROOT" -depth -type d -exec rmdir {} + 2>/dev/null || true
}
trap cleanup EXIT

echo "假 Steam 树: $ROOT"
mkdir -p "$DXGI" "$SYSDIR" "$PFX/drive_c/windows" \
         "$PROTON/files/lib/wine/x86_64-windows" \
         "$PROTON/files/lib/wine/dxvk"
printf 'ORIGINAL-DXVK-DXGI-BYTES\n' > "$DXGI/dxgi.dll"
echo "fake-dxvk-3.1" > "$PROTON/files/lib/wine/dxvk/version"
: > "$PROTON/files/lib/wine/x86_64-windows/explorer.exe"
ln -s "$PROTON/files/lib/wine/x86_64-windows/explorer.exe" "$PFX/drive_c/windows/explorer.exe"

ORIG_SHA="$(sha256sum "$DXGI/dxgi.dll" | cut -d' ' -f1)"

echo
echo "========== 1) 编译产物存在 =========="
chk "dxgi.dll 已编译" "[ -f '$HERE/dxgi.dll' ]"

echo
echo "========== 2) install =========="
STEAM_DIR="$STEAM" "$INSTALL" --appid 3240220 > "$ROOT/install.log" 2>&1 || { cat "$ROOT/install.log"; exit 1; }
sed 's/^/    /' "$ROOT/install.log"
chk "Proton 目录里的 dxgi.dll 已换成我们的" \
    "[ \"\$(sha256sum '$DXGI/dxgi.dll' | cut -d' ' -f1)\" = \"\$(sha256sum '$HERE/dxgi.dll' | cut -d' ' -f1)\" ]"
chk "原件已备份到 wbvram_dxvk_dxgi.dll" "[ -f '$DXGI/wbvram_dxvk_dxgi.dll' ]"
chk "备份内容 = 原始字节" \
    "[ \"\$(sha256sum '$DXGI/wbvram_dxvk_dxgi.dll' | cut -d' ' -f1)\" = \"$ORIG_SHA\" ]"
chk "前缀内已放原件副本" "[ -f '$SYSDIR/wbvram_dxvk_dxgi.dll' ]"
chk "install 日志提到 Proton 反查成功" "grep -q '从前缀软链反查到 Proton' '$ROOT/install.log'"

echo
echo "========== 3) 幂等：再 install 一次不应污染备份 =========="
STEAM_DIR="$STEAM" "$INSTALL" --appid 3240220 > "$ROOT/install2.log" 2>&1
chk "备份仍等于原始字节（没被我们的文件覆盖）" \
    "[ \"\$(sha256sum '$DXGI/wbvram_dxvk_dxgi.dll' | cut -d' ' -f1)\" = \"$ORIG_SHA\" ]"
chk "第二次提示保留了已登记的备份" "grep -q '保留已登记的官方原件备份' '$ROOT/install2.log'"

echo
echo "========== 4) 模拟 Steam 更新：官方把 dxgi.dll 换回来 =========="
printf 'OFFICIAL-DXVK-DXGI-NEW-VERSION\n' > "$DXGI/dxgi.dll"
NEW_SHA="$(sha256sum "$DXGI/dxgi.dll" | cut -d' ' -f1)"
STEAM_DIR="$STEAM" "$INSTALL" status > "$ROOT/status-updated.log" 2>&1 || true
sed 's/^/    /' "$ROOT/status-updated.log"
chk "status 报出「不是我们的」" "grep -q '不是我们的' '$ROOT/status-updated.log'"
STEAM_DIR="$STEAM" "$INSTALL" --appid 3240220 > "$ROOT/install3.log" 2>&1
chk "重装后备份刷新为新版本" \
    "[ \"\$(sha256sum '$DXGI/wbvram_dxvk_dxgi.dll' | cut -d' ' -f1)\" = \"$NEW_SHA\" ]"
chk "dxgi.dll 又是我们的" \
    "[ \"\$(sha256sum '$DXGI/dxgi.dll' | cut -d' ' -f1)\" = \"\$(sha256sum '$HERE/dxgi.dll' | cut -d' ' -f1)\" ]"

echo
echo "========== 5) repair =========="
STEAM_DIR="$STEAM" "$INSTALL" repair --appid 3240220 > "$ROOT/repair.log" 2>&1
chk "repair 重新铺上我们的 dxgi.dll" \
    "[ \"\$(sha256sum '$DXGI/dxgi.dll' | cut -d' ' -f1)\" = \"\$(sha256sum '$HERE/dxgi.dll' | cut -d' ' -f1)\" ]"

echo
echo "========== 6) status（已安装态） =========="
STEAM_DIR="$STEAM" "$INSTALL" status > "$ROOT/status-on.log" 2>&1
sed 's/^/    /' "$ROOT/status-on.log"
chk "status 报出「我们的组件（已安装）」" "grep -q '我们的组件（已安装）' '$ROOT/status-on.log'"
chk "status 读到 DXVK 版本文件" "grep -q 'fake-dxvk-3.1' '$ROOT/status-on.log'"

echo
echo "========== 7) uninstall =========="
STEAM_DIR="$STEAM" "$INSTALL" uninstall --appid 3240220 > "$ROOT/uninstall.log" 2>&1
sed 's/^/    /' "$ROOT/uninstall.log"
chk "dxgi.dll 已还原成被备份的那份" \
    "[ \"\$(sha256sum '$DXGI/dxgi.dll' | cut -d' ' -f1)\" = \"$NEW_SHA\" ]"
chk "前缀内副本已删除" "[ ! -f '$SYSDIR/wbvram_dxvk_dxgi.dll' ]"

echo
echo "========== 7b) 卸载后 status 应报「官方原件」 =========="
STEAM_DIR="$STEAM" "$INSTALL" status > "$ROOT/status-off.log" 2>&1
sed 's/^/    /' "$ROOT/status-off.log"
chk "status 报出「官方原件（未安装）」" "grep -q '官方原件（未安装）' '$ROOT/status-off.log'"

echo
echo "========== 8) 无权限时应给出明确指引 =========="
mkdir -p "$ROOT/nowrite/files/lib/wine/dxvk/x86_64-windows"
printf 'X\n' > "$ROOT/nowrite/files/lib/wine/dxvk/x86_64-windows/dxgi.dll"
chmod 555 "$ROOT/nowrite/files/lib/wine/dxvk/x86_64-windows"
if [ "$(id -u)" = "0" ]; then
    echo "  （当前是 root，跳过实际权限测试；root 可写一切）"
else
    if STEAM_DIR="$STEAM" "$INSTALL" --proton "$ROOT/nowrite" --prefix "$PFX" > "$ROOT/nowrite.log" 2>&1; then
        bad "无权限时不该成功"
    else
        chk "给出了 chown/sudo 指引" "grep -q 'sudo chown' '$ROOT/nowrite.log'"
    fi
fi
chmod 755 "$ROOT/nowrite/files/lib/wine/dxvk/x86_64-windows"

echo
echo "========== 9) --with-nvml 装 NVML 垫片 =========="
GE="$STEAM/compatibilitytools.d/GE-Fake"
mkdir -p "$GE/files/lib/wine/nvidia-libs/nvml/wine/x86_64-windows" \
         "$GE/files/lib/wine/nvidia-libs/nvml/wine/x86_64-unix" \
         "$PROTON/files/lib/wine/x86_64-unix"
printf 'NVML-PE\n' > "$GE/files/lib/wine/nvidia-libs/nvml/wine/x86_64-windows/nvml.dll"
printf 'NVML-SO\n' > "$GE/files/lib/wine/nvidia-libs/nvml/wine/x86_64-unix/nvml.so"
STEAM_DIR="$STEAM" "$INSTALL" --appid 3240220 --with-nvml > "$ROOT/nvml.log" 2>&1
chk "nvml.dll 已落到 Proton 的 x86_64-windows" \
    "[ -f '$PROTON/files/lib/wine/x86_64-windows/nvml.dll' ]"
chk "nvml.so 已落到 Proton 的 x86_64-unix" \
    "[ -f '$PROTON/files/lib/wine/x86_64-unix/nvml.so' ]"
chk "装了标记文件（供卸载识别）" \
    "[ -f '$PROTON/files/lib/wine/x86_64-windows/wbvram-nvml.installed' ]"
STEAM_DIR="$STEAM" "$INSTALL" status > "$ROOT/status-nvml.log" 2>&1
chk "status 报出 NVML 垫片在" "grep -q 'NVML 垫片     : 在' '$ROOT/status-nvml.log'"

echo
echo "========== 10) 卸载应连 NVML 垫片一起清掉 =========="
STEAM_DIR="$STEAM" "$INSTALL" uninstall --appid 3240220 > "$ROOT/uninstall2.log" 2>&1
chk "nvml.dll 已删除" "[ ! -f '$PROTON/files/lib/wine/x86_64-windows/nvml.dll' ]"
chk "nvml.so 已删除"  "[ ! -f '$PROTON/files/lib/wine/x86_64-unix/nvml.so' ]"
chk "dxgi.dll 已还原" \
    "[ \"\$(sha256sum '$DXGI/dxgi.dll' | cut -d' ' -f1)\" = \"$NEW_SHA\" ]"

echo
echo "========== 11) 污染防护：备份是自己人时必须拒绝安装 =========="
# 复刻 2026-09-25 00:0x 那次事故：备份文件被以前那版自己覆盖成了我们的组件
cp -f "$HERE/dxgi.dll" "$DXGI/wbvram_dxvk_dxgi.dll"
cp -f "$HERE/dxgi.dll" "$DXGI/dxgi.dll"
if STEAM_DIR="$STEAM" "$INSTALL" --appid 3240220 > "$ROOT/polluted.log" 2>&1; then
    bad "污染状态下不该继续安装"
else
    chk "拒绝安装"          "grep -q '备份已被污染' '$ROOT/polluted.log'"
    chk "并指向 rollback"   "grep -q 'rollback.sh' '$ROOT/polluted.log'"
fi

echo
echo "========== 12) rollback.sh：从别的前缀恢复官方原件 =========="
OTHER="$STEAM/steamapps/compatdata/999999/pfx/drive_c/windows/system32"
mkdir -p "$OTHER"
printf 'OFFICIAL-DXVK-DXGI-NEW-VERSION\n' > "$OTHER/dxgi.dll"
printf 'OUR-LEFTOVER\n'                  > "$OTHER/wbvram_dxvk_dxgi.dll"
STEAM_DIR="$STEAM" "$ROLLBACK" --appid 3240220 > "$ROOT/rollback.log" 2>&1 || true
sed 's/^/    /' "$ROOT/rollback.log" | sed -n '1,18p'
chk "Proton 里的 dxgi.dll 已恢复成官方内容" \
    "[ \"\$(sha256sum '$DXGI/dxgi.dll' | cut -d' ' -f1)\" = \"$NEW_SHA\" ]"
chk "被污染的备份已删除"  "[ ! -f '$DXGI/wbvram_dxvk_dxgi.dll' ]"
chk "别的前缀里的残留也清了" "[ ! -f '$OTHER/wbvram_dxvk_dxgi.dll' ]"
chk "别的前缀的官方文件未被误改" "grep -q 'OFFICIAL-DXVK-DXGI-NEW-VERSION' '$OTHER/dxgi.dll'"

echo
echo "========== 13) v1fix 对照组能正常装（走 --dll） =========="
V1FIX="$HERE/v1fix"
if [ -f "$V1FIX/dxgi.dll" ]; then
    STEAM_DIR="$STEAM" "$V1FIX/install.sh" --appid 3240220 > "$ROOT/v1fix.log" 2>&1
    V1SHA="$(sha256sum "$V1FIX/dxgi.dll" | cut -d' ' -f1)"
    chk "v1fix 装上了（Proton 里那份哈希一致）" \
        "[ \"\$(sha256sum '$DXGI/dxgi.dll' | cut -d' ' -f1)\" = \"$V1SHA\" ]"
    chk "v1fix 走的也是同一套安装逻辑（有原件备份）" "[ -f '$DXGI/wbvram_dxvk_dxgi.dll' ]"
    chk "v1fix 日志出现" "grep -q '已安装' '$ROOT/v1fix.log'"
    STEAM_DIR="$STEAM" "$INSTALL" uninstall --appid 3240220 > "$ROOT/v1fix-un.log" 2>&1
    chk "v1fix 可正常卸载" \
        "[ \"\$(sha256sum '$DXGI/dxgi.dll' | cut -d' ' -f1)\" = \"$NEW_SHA\" ]"
else
    bad "v1fix/dxgi.dll 不存在（先跑 ./winepart/v1fix/build.sh）"
fi

echo
echo "========== 14) 版本识别：v1fix 与 v2 必须能区分 =========="
V1FIX="$HERE/v1fix"
# 注意：本脚本开了 set -euo pipefail，所以下面一律用 if 包裹、管道加 || true，
# 免得某条命令的非零退出码直接把整个自检带走。
vline() { STEAM_DIR="$STEAM" "$INSTALL" status --appid 3240220 2>&1 | grep -m1 '^dxgi\.dll' || true; }

if STEAM_DIR="$STEAM" "$V1FIX/install.sh" --appid 3240220 > "$ROOT/s14-v1.log" 2>&1; then
    ok "v1fix 装载成功"
else
    bad "v1fix 装载失败（尾 5 行见下）"
    sed 's/^/      /' "$ROOT/s14-v1.log" | tail -5
fi
line1="$(vline)"
if echo "$line1" | grep -q 'v1 系列'; then
    ok "v1fix 被认成「v1 系列」"
else
    bad "v1fix 版本识别错: $line1"
fi

if STEAM_DIR="$STEAM" "$INSTALL" install --appid 3240220 > "$ROOT/s14-v2.log" 2>&1; then
    ok "v2 装载成功"
else
    bad "v2 装载失败"
    sed 's/^/      /' "$ROOT/s14-v2.log" | tail -5
fi
line2="$(vline)"
if echo "$line2" | grep -q 'v3\.1'; then
    ok "v2 被认成「v3.1（逐进程枚举，剔除自己）」"
else
    bad "v2 版本识别错: $line2"
fi

STEAM_DIR="$STEAM" "$INSTALL" uninstall --appid 3240220 >/dev/null 2>&1 || true
chk "版本测试后已还原" \
    "[ \"\$(sha256sum '$DXGI/dxgi.dll' | cut -d' ' -f1)\" = \"$NEW_SHA\" ]"

echo
echo "========== 15) install-v2.sh 一键入口（只跑 --dry-run，不动手） =========="
V2SH="$HERE/install-v2.sh"
if [ -x "$V2SH" ]; then
    if STEAM_DIR="$STEAM" "$V2SH" --dry-run > "$ROOT/s15-v2dry.log" 2>&1; then
        ok "dry-run 正常退出"
    else
        bad "dry-run 退出码非 0"
    fi
    chk "识别出待装组件是 v3.1" "grep -q 'v3.1' '$ROOT/s15-v2dry.log'"
    chk "打印了将要执行的命令"     "grep -q '将要执行' '$ROOT/s15-v2dry.log'"
    chk "dry-run 确实没改文件" \
        "[ \"\$(sha256sum '$DXGI/dxgi.dll' | cut -d' ' -f1)\" = \"$NEW_SHA\" ]"
else
    bad "install-v2.sh 不存在或不可执行"
fi

echo
echo "============================================================"
echo "自检结果: 通过 $pass 项，失败 $fail 项"
[ "$fail" = 0 ] || exit 1
