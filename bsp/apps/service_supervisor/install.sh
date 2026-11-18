#!/bin/sh
#
# install.sh - 安装 Service Supervisor（与 G3 镜像构建脚本统一使用 service_supervisor_run）
#
# 用法：
#   sh install.sh [/opt/smartgateway]
#
# 设计约定：
#   1. 安装 $ROOT/bin/service_supervisor_run 与 /etc/init.d/S99smartgateway。
#   2. 若 $INITTAB 已包含统一的 service_supervisor_run respawn 条目（如 G3 签名镜像），
#      则保持 $INITTAB 只读不改动（绝不生成备份或破坏签名清单摘要）。
#   3. 仅当 $INITTAB 尚未注册统一 respawn 条目时（如独立/全新环境），才写入统一样式的
#      sgw0::respawn:...service_supervisor_run（SysV init）或 ::respawn:...（BusyBox init），
#      且后续 S99smartgateway start/stop 仅通过运行时 $STOP_FLAG 控制，不再修改 $INITTAB。
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
RELEASES="$ROOT/releases"

MARKER="# smartgateway-supervisor-respawn"

say() { printf '%s\n' "$*"; }

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

if [ "$INITTAB" = "/etc/inittab" ] || [ "$INITD" = "/etc/init.d/S99smartgateway" ]; then
    [ "$(id -u)" = "0" ] || fail "must run as root"
fi

say "== 安装 Service Supervisor 到 $ROOT =="

# 1. 目录结构
mkdir -p "$ROOT/bin" "$RUN_DIR" "$LOGS" "$RELEASES"
[ -x "$BIN" ] || fail "$BIN not found; copy the binary first"
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

# 3. 安装统一的 S99smartgateway 与 service_supervisor_run
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

[ -f "$SCRIPT_DIR/service_supervisor_run" ] || fail "找不到 $SCRIPT_DIR/service_supervisor_run"
cp "$SCRIPT_DIR/service_supervisor_run" "$RUN_WRAPPER"
chmod 0700 "$RUN_WRAPPER"
say "已安装 $RUN_WRAPPER"

if [ -f "$SCRIPT_DIR/S99smartgateway" ]; then
    mkdir -p "$(dirname "$INITD")"
    cp "$SCRIPT_DIR/S99smartgateway" "$INITD"
    chmod 0755 "$INITD"
    say "已安装 $INITD"
elif [ -f "$INITD" ]; then
    say "使用已有 $INITD"
else
    fail "找不到 S99smartgateway（与 install.sh 同目录）"
fi

# 4. 统一 respawn 条目：已固定存在则不改动 $INITTAB（保护签名清单摘要）
INITTAB_CHANGED=0
if grep -q ':initdefault:' "$INITTAB" 2>/dev/null; then
    RESPAWN_PREFIX="sgw0::respawn:"
else
    RESPAWN_PREFIX="::respawn:"
fi
PROCESS_FIELD="/bin/sh -c 'SMARTGATEWAY_RESPAWN=1 exec $RUN_WRAPPER'"
UNIFIED_ENTRY="${RESPAWN_PREFIX}${PROCESS_FIELD}"

if grep -qF "$RUN_WRAPPER" "$INITTAB" 2>/dev/null && \
   grep -Eq '^[^#]*:respawn:.*service_supervisor_run' "$INITTAB" 2>/dev/null; then
    say "检测到 $INITTAB 已存在统一 respawn 条目，保持只读不修改"
else
    if [ "${#PROCESS_FIELD}" -gt 127 ] && [ "$RESPAWN_PREFIX" = "sgw0::respawn:" ]; then
        fail "process field exceeds sysvinit 127-char limit (${#PROCESS_FIELD}): $PROCESS_FIELD"
    fi
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

# 5. 让 init 接管（若从旧版直连进程升级则先停旧实例再通过 S99smartgateway start 恢复）
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
