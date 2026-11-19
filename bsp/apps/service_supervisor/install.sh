#!/bin/sh
#
# install.sh - 安装 Service Supervisor（与 G3 镜像构建脚本统一使用 service_supervisor_run）
#
# 用法：
#   sh install.sh [/opt/smartgateway]
#
# 设计约定：
#   1. 在任何写盘前检测是否处于已固定/签名镜像环境（inittab 含 service_supervisor_run，或
#      策略目录存在 r3c-policy.conf / ota-public.pem）。
#   2. 若处于已固定/签名镜像环境，先严格核对 inittab、S99smartgateway 与 service_supervisor_run
#      是否与期望/安装包完全一致：全部一致则跳过文件写入以保护签名清单摘要；任一不一致
#      则立即失败退出，且不修改 inittab、S99smartgateway 或 service_supervisor_run。
#   3. 仅在全新/非固定独立环境执行常规安装并写入统一样式的 respawn 条目。
#
set -e

ROOT=${1:-/opt/smartgateway}
BIN="$ROOT/bin/service_supervisor"
RUN_WRAPPER="$ROOT/bin/service_supervisor_run"
RUN_DIR="$ROOT/run"
SOCK="$RUN_DIR/supervisor.sock"
LOGS="$ROOT/logs"
CONF="$ROOT/service.conf"
INITTAB=${SMARTGATEWAY_INITTAB:-/etc/inittab}
INITD=${SMARTGATEWAY_INITD:-/etc/init.d/S99smartgateway}
INIT_PID=${SMARTGATEWAY_INIT_PID:-1}
if [ "${SMARTGATEWAY_POLICY_DIR+set}" = "set" ]; then
    POLICY_DIR="$SMARTGATEWAY_POLICY_DIR"
elif [ "$INITTAB" = "/etc/inittab" ]; then
    POLICY_DIR="/etc/smartgateway"
else
    POLICY_DIR=""
fi
RELEASES="$ROOT/releases"

MARKER="# smartgateway-supervisor-respawn"

say() { printf '%s\n' "$*"; }

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

files_identical() {
    [ -f "$1" ] && [ -f "$2" ] && cmp -s "$1" "$2"
}

if [ "$INITTAB" = "/etc/inittab" ] || [ "$INITD" = "/etc/init.d/S99smartgateway" ]; then
    [ "$(id -u)" = "0" ] || fail "must run as root"
fi

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

# 只读前置校验（发生在任何 mkdir / cp / chmod / mv / init-config 之前）
[ -x "$BIN" ] || fail "$BIN not found; copy the binary first"
[ -f "$SCRIPT_DIR/service_supervisor_run" ] || fail "找不到 $SCRIPT_DIR/service_supervisor_run"
if [ ! -f "$SCRIPT_DIR/S99smartgateway" ] && [ ! -f "$INITD" ]; then
    fail "找不到 S99smartgateway（与 install.sh 同目录）"
fi

if grep -q ':initdefault:' "$INITTAB" 2>/dev/null; then
    RESPAWN_PREFIX="sgw0::respawn:"
else
    RESPAWN_PREFIX="::respawn:"
fi
PROCESS_FIELD="/bin/sh -c 'SMARTGATEWAY_RESPAWN=1 exec $RUN_WRAPPER'"
UNIFIED_ENTRY="${RESPAWN_PREFIX}${PROCESS_FIELD}"

IS_PREPINNED=0
if grep -Eq '^[^#]*:respawn:.*service_supervisor_run' "$INITTAB" 2>/dev/null; then
    IS_PREPINNED=1
fi

HAS_SIGNED_POLICY=0
if [ -n "$POLICY_DIR" ]; then
    if [ -f "$POLICY_DIR/r3c-policy.conf" ] || [ -f "$POLICY_DIR/ota-public.pem" ]; then
        HAS_SIGNED_POLICY=1
    fi
fi

SKIP_FILE_INSTALL=0
INITTAB_CHANGED=0

if [ "$IS_PREPINNED" = "1" ] || [ "$HAS_SIGNED_POLICY" = "1" ]; then
    if ! grep -qxF "$UNIFIED_ENTRY" "$INITTAB" 2>/dev/null; then
        fail "$INITTAB 与预期统一 respawn 条目不一致（未改动 $INITTAB 与 $INITD）"
    fi
    if [ ! -f "$SCRIPT_DIR/S99smartgateway" ] || ! files_identical "$SCRIPT_DIR/S99smartgateway" "$INITD"; then
        fail "$INITD 与安装包中的 S99smartgateway 版本不一致（拒绝覆盖 $INITD；未改动 $INITTAB 与 $INITD）"
    fi
    if ! files_identical "$SCRIPT_DIR/service_supervisor_run" "$RUN_WRAPPER"; then
        fail "$RUN_WRAPPER 与安装包中的 service_supervisor_run 版本不一致（拒绝覆盖 $RUN_WRAPPER；未改动 $INITTAB 与 $INITD）"
    fi
    SKIP_FILE_INSTALL=1
else
    if [ "${#PROCESS_FIELD}" -gt 127 ] && [ "$RESPAWN_PREFIX" = "sgw0::respawn:" ]; then
        fail "process field exceeds sysvinit 127-char limit (${#PROCESS_FIELD}): $PROCESS_FIELD"
    fi
fi

say "== 安装 Service Supervisor 到 $ROOT =="

if [ "$SKIP_FILE_INSTALL" = "1" ]; then
    say "检测到已固定/签名镜像且 inittab、S99smartgateway 与 service_supervisor_run 内容完全一致，跳过文件安装以保护签名清单摘要"
    mkdir -p "$RUN_DIR" "$LOGS"
    if [ -f "$CONF" ]; then
        say "配置已存在，保留：$CONF"
    else
        "$BIN" init-config --root "$ROOT" \
            --executable "$ROOT/current/smart_gateway" >/dev/null
        say "已写出默认配置：$CONF"
    fi
else
    # 1. 目录结构
    mkdir -p "$ROOT/bin" "$RUN_DIR" "$LOGS" "$RELEASES"
    [ -d "$RELEASES" ] || fail "cannot create $RELEASES"
    say "目录就绪"

    # 2. 配置（不覆盖已有文件）
    if [ -f "$CONF" ]; then
        say "配置已存在，保留：$CONF"
    else
        "$BIN" init-config --root "$ROOT" \
            --executable "$ROOT/current/smart_gateway" >/dev/null
        say "已写出默认配置：$CONF"
    fi

    # 3. 安装统一的 service_supervisor_run 与 S99smartgateway
    cp "$SCRIPT_DIR/service_supervisor_run" "$RUN_WRAPPER"
    chmod 0700 "$RUN_WRAPPER"
    say "已安装 $RUN_WRAPPER"

    if [ -f "$SCRIPT_DIR/S99smartgateway" ]; then
        mkdir -p "$(dirname "$INITD")"
        cp "$SCRIPT_DIR/S99smartgateway" "$INITD"
        chmod 0755 "$INITD"
        say "已安装 $INITD"
    else
        say "使用已有 $INITD"
    fi

    # 4. 写入统一 respawn 条目
    if [ -f "$INITTAB" ]; then
        cp -a "$INITTAB" "$INITTAB.bak.$(date +%s)"
    fi
    TMP=$(mktemp)
    if [ -f "$INITTAB" ]; then
        grep -v "$MARKER" "$INITTAB" 2>/dev/null \
            | grep -v '^# G4b:' \
            | grep -Ev '^[^#]*:respawn:.*service_supervisor' \
            > "$TMP" || true
    fi
    cat >> "$TMP" <<EOF

$MARKER
# G4b: init respawns service_supervisor_run automatically (runtime pause via STOP_FLAG).
$UNIFIED_ENTRY
EOF
    mv "$TMP" "$INITTAB"
    sync
    INITTAB_CHANGED=1
    say "已在 $INITTAB 注册统一 respawn 条目（后续由 S99smartgateway 运行时状态控制）"
fi

# 5. 让 init 接管
export SMARTGATEWAY_ROOT="$ROOT"
export SMARTGATEWAY_INITTAB="$INITTAB"
export SMARTGATEWAY_INIT_PID="$INIT_PID"

if [ "$INITTAB_CHANGED" = "1" ]; then
    for p in /proc/[0-9]*; do
        e=$(readlink "$p/exe" 2>/dev/null) || continue
        case "$e" in
            */service_supervisor) ;;
            *) continue ;;
        esac
        cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
        case "$cmd" in
            *"run --root $ROOT"*)
                kill -TERM "${p#/proc/}" 2>/dev/null || true
                ;;
        esac
    done
    sleep 0.5
    rm -f "$SOCK"
    kill -HUP "$INIT_PID" 2>/dev/null || fail "cannot signal init ($INIT_PID)"
fi

"$INITD" start

# 6. 验证
say ""
say "-- 验证 --"
ok=1
if [ -S "$SOCK" ]; then
    say "  OK   supervisor socket 存在"
else
    say "  FAIL supervisor socket 不存在"
    ok=0
fi

if "$INITD" status >/dev/null 2>&1; then
    "$INITD" status | sed 's/^/  /'
else
    say "  FAIL status 检查失败"
    ok=0
fi

if [ "$ok" -eq 1 ]; then
    say ""
    say "安装完成。"
    exit 0
fi

fail "安装验证失败，请检查 $LOGS/supervisor.log"
