#!/system/bin/sh
# kpm.sh — KPM 构建/部署/控制快捷脚本
#
# 用法（普通用户运行即可，需要 root 的步骤自动 su）：
#   ./kpm.sh build [模块目录]         # make KP_DIR=<KP_DIR>
#   ./kpm.sh deploy <x.kpm> [args]    # push 到 /data/local/tmp 并 ksud kpm load
#   ./kpm.sh load <x.kpm> [args]      # 仅 load（先手动 push 过）
#   ./kpm.sh ctl <name> <args...>     # 发控制命令（打印返回值）
#   ./kpm.sh list|num|ver             # 状态
#   ./kpm.sh info <name>
#   ./kpm.sh unload <name>
#   ./kpm.sh log [grep-pattern]       # dmesg 过滤（默认 KPM 相关）
#
# 环境变量: KP_DIR (默认 ~/KP-SUKISU), KSUD (默认 /data/adb/ksud)

set -u
KP_DIR="${KP_DIR:-$HOME/KP-SUKISU}"
KSUD="${KSUD:-/data/adb/ksud}"

as_root() {
    if [ "$(id -u)" = "0" ]; then
        "$@"
    else
        su -c "$(printf '%s ' "$@")"
    fi
}

cmd="$1"; shift 2>/dev/null || true

case "$cmd" in
    build)
        dir="${1:-.}"
        make -C "$dir" KP_DIR="$KP_DIR"
        ;;
    deploy)
        f="$1"; shift
        [ -f "$f" ] || { echo "no such file: $f" >&2; exit 1; }
        base="$(basename "$f")"
        as_root cp "$f" /data/local/tmp/"$base" || exit 1
        as_root "$KSUD" kpm load /data/local/tmp/"$base" "$@"
        ;;
    load)
        f="$1"; shift
        as_root "$KSUD" kpm load "$f" "$@"
        ;;
    ctl)
        name="$1"; shift
        as_root "$KSUD" kpm control "$name" "$*"
        ;;
    list|num|ver)
        [ "$cmd" = "ver" ] && cmd=version
        as_root "$KSUD" kpm "$cmd"
        ;;
    info|unload)
        as_root "$KSUD" kpm "$cmd" "$1"
        ;;
    log)
        pat="${1:-KPM|kpm|load_module|unload_module|module_control0|unknown symbol}"
        as_root dmesg | grep -E "$pat" | tail -40
        ;;
    *)
        sed -n '2,20p' "$0"
        exit 1
        ;;
esac
