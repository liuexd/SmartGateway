#!/bin/sh
#
# install.sh - 把 Service Supervisor 安装到板端（G4a + G4b）
#
# 用法（在板端执行）：
#   sh install.sh /opt/smartgateway
#
# 它做四件事：
#   1. 创建目录结构
#   2. 安装 init 脚本
#   3. **向 /etc/inittab 注册 respawn 条目**（G4b）
#   4. 触发现有实例按新配置重新拉起
#
# ============================================================
# 关于 G4b 与 respawn
# ============================================================
#
# 问题（S5 遗留 L12）：
#   S5 只由 rcS 启动一次 supervisor。它崩溃后没有任何机制重拉，
#   服务消失直到下次重启。这不是"崩溃自愈"。
#
# 解决：
#   在 /etc/inittab 加一行
#       ::respawn:/opt/smartgateway/bin/service_supervisor run --root ...
#   BusyBox init 会在该进程退出后自动重新执行它。
#
# 实测确认（板端 i.MX6ULL / BusyBox init）：
#   - kill -HUP 1 会重载 inittab 并立即应用新条目
#   - kill -9 掉被 respawn 的进程后，init 会重新拉起（新 pid）
#   - init 对快速退出的进程有节流（实测约 1 次/秒），
#     不会因为"启动即崩溃"而把 CPU 烧满
#
# ============================================================
# 为什么必须与 start-stop-daemon 解耦
# ============================================================
#
# respawn 之后，supervisor 由 init fork，不再是 init 脚本
# fork 的。此时：
#   - start-stop-daemon --make-pidfile 写出的 pid 无效
#   - 用 pidfile 判断"是否在运行"会得出错误结论
# 因此 S99smartgateway 已改为通过 socket 连通性判断，
# 本脚本也不再生成任何 supervisor pidfile。
#
# ============================================================
# 幂等性
# ============================================================
#
# 本脚本可重复执行：
#   - 目录创建用 mkdir -p
#   - inittab 条目先删除旧的再加，不会重复
#   - 配置文件不覆盖（保留现场调整）
#
set -e

ROOT=${1:-/opt/smartgateway}
BIN="$ROOT/bin/service_supervisor"
RUN_DIR="$ROOT/run"
LOGS="$ROOT/logs"
CONF="$ROOT/service.conf"
INITTAB=/etc/inittab
INITD=/etc/init.d/S99smartgateway
RELEASES="$ROOT/releases"

# respawn 条目的标记，用于幂等替换
MARKER="# smartgateway-supervisor-respawn"

say() { printf '%s\n' "$*"; }

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

[ "$(id -u)" = "0" ] || fail "must run as root"

say "== 安装 Service Supervisor 到 $ROOT =="

# ------------------------------------------------------------
# 1. 目录结构
# ------------------------------------------------------------
mkdir -p "$ROOT/bin" "$RUN_DIR" "$LOGS" "$RELEASES"

[ -x "$BIN" ] || fail "$BIN not found; copy the ARM binary first"
[ -d "$RELEASES" ] || fail "cannot create $RELEASES"

say "目录就绪"

# ------------------------------------------------------------
# 2. 配置（不覆盖已有文件）
# ------------------------------------------------------------
if [ -f "$CONF" ]; then
    say "配置已存在，保留：$CONF"
else
    "$BIN" init-config --root "$ROOT" \
        --executable "$ROOT/current/smart_gateway" >/dev/null
    say "已写出默认配置：$CONF"
fi

# ------------------------------------------------------------
# 3. init 脚本
# ------------------------------------------------------------
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

if [ -f "$SCRIPT_DIR/S99smartgateway" ]; then
    cp "$SCRIPT_DIR/S99smartgateway" "$INITD"
    chmod 0755 "$INITD"
    say "已安装 $INITD"
elif [ -f "$INITD" ]; then
    say "使用已有 $INITD"
else
    fail "找不到 S99smartgateway（与 install.sh 同目录）"
fi

# ------------------------------------------------------------
# 4. inittab respawn 条目（G4b）
# ------------------------------------------------------------
#
# 关键细节：先备份，再就地重写。
#
# 不用 `echo >> inittab` 的原因：那会造成重复条目，
# 而重复的 respawn 会让 init 拉起两个 supervisor
# （第二个因 socket 冲突退出，于是陷入"退出-重拉"循环）。
#
if [ -f "$INITTAB" ]; then
    cp -a "$INITTAB" "$INITTAB.bak.$(date +%s)"
fi

TMP=$(mktemp)

#
# 清理旧条目时刻意"宽进"：不只用 MARKER 匹配。
#
# 原因（这次踩到了）：早期版本没有 marker 注释，只靠
# MARKER 过滤会留下旧行，于是 inittab 里同时存在两条 respawn，
# init 便拉起两个 supervisor —— 第二个因 socket 冲突退出，
# 然后被反复重拉。
#
# 因此这里按"任何指向本 BIN 的 respawn 行"统统删掉，
# 无论它长什么样、有没有 marker。
#
grep -v "$MARKER" "$INITTAB" 2>/dev/null \
    | grep -v "^::respawn:.*service_supervisor" \
    > "$TMP" || true

cat >> "$TMP" <<EOF

$MARKER
# G4b：supervisor 崩溃后由 init 自动重新拉起。
#
# 实测（BusyBox init / i.MX6ULL）：
#   kill -HUP 1         -> 立即应用新条目
#   kill -9 supervisor  -> init 重新执行该命令
# SMARTGATEWAY_RESPAWN=1 告诉 supervisor 它处于 init 监督下。
# 在这个模式下，若发现上一代 supervisor 留下的孤儿 Gateway，
# 它会**领养**而不是拒绝启动 —— 否则 init 会无限重拉一个
# 必然失败的命令，形成永久重启循环。
#
# 为什么要包一层 sh -c：
#   respawn 直接 exec 命令，没有 shell 解析 "VAR=1 cmd" 或
#   重定向。不重定向的后果是 supervisor 的启动输出（包括
#   "adopting orphaned gateway" 这类关键诊断）全部丢失，
#   排查时看到的是一个空日志文件。
#
#   用 "exec" 而不是直接运行：exec 让 sh 被 supervisor 取代，
#   于是 init 监督的是 supervisor 本身而不是中间那层 sh。
#   若不 exec，supervisor 崩溃时退出的是 sh，init 仍会重拉，
#   但 PID 关系与信号传递都多绕一层，容易误判。
::respawn:/bin/sh -c 'SMARTGATEWAY_RESPAWN=1 exec $BIN run --root $ROOT >> $LOGS/supervisor.log 2>&1'
EOF

mv "$TMP" "$INITTAB"
sync

say "已在 $INITTAB 注册 respawn 条目"
say "（此后由 S99smartgateway start 添加、stop 移除）"

# ------------------------------------------------------------
# 5. 让新配置立即生效
# ------------------------------------------------------------
#
# 先停掉由 rcS 启动的旧实例，再让 init 通过 respawn 接管。
#
# 为什么不直接 kill -HUP 1 就完事：
#   已经存在的 supervisor 是 rcS 的子进程（父已退出，被 init
#   收养），HUP 不会改变它的父子关系。必须先停掉，让 respawn
#   条目从头接管，ppid 才会稳定是 1。
#
if [ -S "$RUN_DIR/supervisor.sock" ]; then
    say "停止已有 supervisor，交给 respawn 接管"
    "$INITD" stop >/dev/null 2>&1 || true
fi

kill -HUP 1 2>/dev/null || fail "cannot signal init"
sleep 2

# ------------------------------------------------------------
# 6. 验证
# ------------------------------------------------------------
say ""
say "-- 验证 --"

ok=1

if [ -S "$RUN_DIR/supervisor.sock" ]; then
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
    say ""
    say "验证 G4b（崩溃自愈）："
    say "  1. 找到 supervisor 的 pid："
    say "     for p in /proc/[0-9]*; do readlink \$p/exe 2>/dev/null | grep -q service_supervisor && echo \${p#/proc/}; done"
    say "  2. kill -9 <该 pid>，等 1-2 秒"
    say "  3. $INITD status   # 应重新出现，且 ppid=1"
    exit 0
fi

fail "安装验证失败，请检查 $LOGS/supervisor.log"
