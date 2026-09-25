#!/usr/bin/env bash
# 部署 / 卸载 drop-in 版 dxgi.dll
#
#   ./dropin/deploy.sh                     # 装到 GTA V Enhanced 根目录
#   ./dropin/deploy.sh uninstall           # 删除
#   GAME_DIR=/path/to/game ./dropin/deploy.sh     # 指定别的游戏
#
# ⚠️ GTA Online 带 BattlEye。自定义 DLL 进游戏目录有被判定为篡改的风险，
#    建议只在故事模式使用，进线上前先 uninstall。

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DLL="$HERE/dxgi.dll"
GAME="${GAME_DIR:-$HOME/.local/share/Steam/steamapps/common/Grand Theft Auto V Enhanced}"

if [ "${1:-}" = "uninstall" ]; then
    if [ -f "$GAME/dxgi.dll" ]; then
        rm -f "$GAME/dxgi.dll"
        echo "已删除 $GAME/dxgi.dll"
    else
        echo "那里没有 dxgi.dll"
    fi
    [ -f "$GAME/wbvram.log" ] && echo "（日志保留在 $GAME/wbvram.log，可自行删除）"
    exit 0
fi

if [ ! -d "$GAME" ]; then
    echo "找不到游戏目录: $GAME"
    echo "用 GAME_DIR=... 指定，或先确认路径"
    exit 1
fi

if [ ! -f "$DLL" ]; then
    echo "先编译： ./dropin/build.sh"
    exit 1
fi

if [ -f "$GAME/dxgi.dll" ]; then
    if cmp -s "$DLL" "$GAME/dxgi.dll"; then
        echo "游戏目录里已经是最新版本，重新覆盖一次"
    else
        echo "注意：游戏目录里已有一个 dxgi.dll，将被覆盖。"
        echo "      如果你想保留它，先自己备份。"
    fi
fi

cat <<'WARN'
---------------------------------------------------------------
⚠️  GTA Online 带 BattlEye，自定义 DLL 有被反作弊判定为篡改的风险。
    建议只在故事模式（单机）使用；进线上前先跑 uninstall。
---------------------------------------------------------------
WARN

read -r -p "确认继续？[y/N] " ans
case "$ans" in
    y|Y|yes|YES) ;;
    *) echo "已取消"; exit 0 ;;
esac

cp -f "$DLL" "$GAME/dxgi.dll"
echo
echo "已部署: $GAME/dxgi.dll"
echo "运行一次游戏后查看日志: $GAME/wbvram.log"
echo "卸载: $HERE/deploy.sh uninstall"
