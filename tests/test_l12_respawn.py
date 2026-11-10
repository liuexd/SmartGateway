#!/usr/bin/env python3
"""L12 验收：supervisor 崩溃自愈（G4b）

问题
----
S5 只由 rcS 启动一次 supervisor。它崩溃后没有任何机制重拉，
服务消失直到下次重启。这不是崩溃自愈。

解决
----
在 /etc/inittab 注册 respawn 条目，由 BusyBox init 监督。
本脚本验证这条链路在**真实板端**成立。

为什么必须板端测
----------------
respawn 是 init 的行为，x86 上没有 BusyBox init。
用 mock 验证「respawn 逻辑」等于验证自己写的 mock，
不能说明任何问题。

前提（实测已确认）
------------------
1. kill -HUP 1 会重载 inittab 并立即应用新条目
2. kill -9 掉被 respawn 的进程后 init 会重新拉起
3. init 对快速退出的 respawn 进程有节流（约 1 次/秒），
   不会因"启动即崩溃"烧满 CPU

用法（在开发机执行，通过 SSH 操作板端）:
  python3 tests/test_l12_respawn.py
  python3 tests/test_l12_respawn.py --host root@192.168.10.3
"""

import argparse
import re
import subprocess
import sys
import time

BOARD = "root@192.168.10.3"
ROOT = "/opt/smartgateway"
INITD = "/etc/init.d/S99smartgateway"
INITTAB = "/etc/inittab"
MARKER = "# smartgateway-supervisor-respawn"

results = []


def record(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}"
          + (f": {detail}" if detail else ""), flush=True)


def ssh(cmd, timeout=60, host=BOARD):
    """执行远端命令。

    BatchMode 避免卡在密码提示上。stderr 合并进 stdout：
    板端的错误信息往往在 stderr，分开会让诊断变困难。
    """
    r = subprocess.run(
        ["ssh", "-o", "BatchMode=yes",
         "-o", "ConnectTimeout=8", host, cmd],
        capture_output=True, text=True, timeout=timeout)
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def strip_ssh_noise(text):
    """过滤 OpenSSH 的版本告警，只保留命令输出。"""
    keep = []
    for line in text.splitlines():
        if "post-quantum" in line:
            continue
        if line.startswith("** WARNING") or line.startswith("** This session"):
            continue
        if line.startswith("** The server may need"):
            continue
        keep.append(line)
    return "\n".join(keep)


def supervisor_pids(host=BOARD):
    """列出本 ROOT 的 supervisor 进程。

    只按可执行文件 + 命令行里的 --root 精确匹配：
    板端可能同时存在多份安装，不能误伤。
    """
    rc, out = ssh(
        'for p in /proc/[0-9]*; do '
        'e=$(readlink "$p/exe" 2>/dev/null) || continue; '
        'case "$e" in */service_supervisor) ;; *) continue ;; esac; '
        f'cmd=$(tr "\\0" " " < "$p/cmdline" 2>/dev/null) || continue; '
        f'case "$cmd" in *"run --root {ROOT}"*) '
        'echo "${p#/proc/} $(awk \'{print $4}\' $p/stat 2>/dev/null)";; '
        'esac; done', host=host)
    pids = []
    for line in strip_ssh_noise(out).splitlines():
        parts = line.split()
        if len(parts) >= 1 and parts[0].isdigit():
            ppid = parts[1] if len(parts) > 1 else "?"
            pids.append((int(parts[0]), ppid))
    return pids


def test_respawn_entry_present(host):
    """inittab 必须有且只有一条 respawn 条目。"""
    print("\n" + "=" * 72)
    print("检查一：inittab respawn 条目")
    print("=" * 72)

    rc, out = ssh(f"cat {INITTAB}", host=host)
    out = strip_ssh_noise(out)
    print(out, flush=True)

    lines = [l for l in out.splitlines() if l.strip().startswith("::respawn:")]
    matching = [l for l in lines if "service_supervisor" in l]

    record("inittab 含 supervisor 的 respawn 条目",
           len(matching) == 1, f"count={len(matching)}")

    if not matching:
        return False

    entry = matching[0]

    #
    # SMARTGATEWAY_RESPAWN=1 是必需的：它让 supervisor 在发现
    # 孤儿 Gateway 时领养而不是拒绝启动。没有它，init 会无限
    # 重拉一个必然失败的命令。
    #
    record("respawn 条目传 SMARTGATEWAY_RESPAWN=1",
           "SMARTGATEWAY_RESPAWN=1" in entry, entry.strip()[:80])

    record("respawn 条目的 --root 与安装路径一致",
           f"--root {ROOT}" in entry, entry.strip()[:80])

    #
    # 只允许一条。重复条目会让 init 拉起两个 supervisor，
    # 第二个因 socket 冲突退出，然后陷入"退出-重拉"循环。
    #
    record("没有重复的 respawn 条目", len(matching) == 1)

    return True


def test_supervisor_under_init(host):
    """supervisor 的父进程必须是 init(1)。"""
    print("\n" + "=" * 72)
    print("检查二：supervisor 由 init 监督（ppid=1）")
    print("=" * 72)

    pids = supervisor_pids(host)
    record("找到 supervisor 进程", len(pids) >= 1, f"pids={pids}")

    if not pids:
        return False

    #
    # 只应有一个。多于一个说明 init 与 S99 各起了一个。
    #
    record("只有一个 supervisor 实例", len(pids) == 1, f"pids={pids}")

    pid, ppid = pids[0]

    #
    # ppid 必须为 1。
    #
    # 若 ppid 是别的（例如 rcS 的残留），说明它是被脚本 fork 的，
    # 而不是被 init respawn 管理的 —— 那种进程崩了不会自愈。
    #
    record("supervisor 的父进程是 init(1)", ppid == "1",
           f"pid={pid} ppid={ppid}")

    return True


def test_crash_recovery(host):
    """kill -9 supervisor 后必须自动重新拉起。"""
    print("\n" + "=" * 72)
    print("检查三：supervisor 崩溃后自动恢复（G4b 核心）")
    print("=" * 72)

    pids = supervisor_pids(host)
    if not pids:
        record("前置：supervisor 在运行", False)
        return False

    old_pid = pids[0][0]
    record("前置：supervisor 在运行", True, f"pid={old_pid}")

    # 先确认业务可用，这样恢复后可以对比
    rc, out = ssh(f"{INITD} status", host=host)
    before_status = strip_ssh_noise(out)
    print(before_status, flush=True)

    rc, out = ssh(f"kill -9 {old_pid}", host=host)
    record("已发送 SIGKILL", True, f"pid={old_pid}")

    #
    # 等 init 重新拉起。
    #
    # 实测 init 对 respawn 有节流（约 1 次/秒），所以不能立即检查。
    # 但也不能无限等：如果 respawn 没配好，等再久也不会有新进程。
    #
    deadline = time.monotonic() + 20
    new_pids = []

    while time.monotonic() < deadline:
        new_pids = supervisor_pids(host)

        if new_pids and new_pids[0][0] != old_pid:
            break

        time.sleep(0.5)

    record("supervisor 被重新拉起", bool(new_pids), f"pids={new_pids}")

    if not new_pids:
        return False

    new_pid, new_ppid = new_pids[0]

    #
    # 必须是**新**进程。若 pid 没变，说明 kill -9 没生效，
    # 那样这条测试等于什么都没验证。
    #
    record("是新进程（pid 已改变）", new_pid != old_pid,
           f"{old_pid} -> {new_pid}")

    record("新进程仍由 init 监督（ppid=1）", new_ppid == "1",
           f"pid={new_pid} ppid={new_ppid}")

    # socket 应已重建
    deadline = time.monotonic() + 10
    up = False
    while time.monotonic() < deadline:
        rc, out = ssh(f"{INITD} status", host=host)
        if rc == 0 and "running" in strip_ssh_noise(out):
            up = True
            break
        time.sleep(0.5)

    record("恢复后 supervisor 可服务", up)

    return True


def test_orphan_adoption(host):
    """supervisor 崩溃后孤儿 Gateway 应被领养，而不是导致启动失败。

    这是 respawn 模式下最容易出问题的地方：
    上一代 supervisor 被 kill -9，它启动的 Gateway 成了孤儿
    但仍在跑，pidfile 也在。新 supervisor 若不领养而是拒绝启动，
    init 就会无限重拉一个必然失败的命令。
    """
    print("\n" + "=" * 72)
    print("检查四：孤儿 Gateway 被领养（避免 respawn 死循环）")
    print("=" * 72)

    # 确保 Gateway 在跑
    rc, out = ssh(f"{INITD} start", host=host)
    time.sleep(2)

    rc, out = ssh(f"{INITD} status", host=host)
    status = strip_ssh_noise(out)
    print(status, flush=True)

    if "running" not in status:
        #
        # Gateway 起不来（例如串口缺失）时无法构造孤儿场景。
        # 如实记录为未覆盖，不当成 PASS。
        #
        record("前置：Gateway 在运行", False,
               "无法构造孤儿场景，本项未验证")
        return False

    record("前置：Gateway 在运行", True)

    pids = supervisor_pids(host)
    if not pids:
        record("前置：supervisor 在运行", False)
        return False

    # 只杀 supervisor，不动 Gateway —— 制造孤儿
    rc, out = ssh(f"kill -9 {pids[0][0]}", host=host)
    record("已杀死 supervisor（Gateway 成为孤儿）", True)

    #
    # 等 init 拉起新 supervisor 并领养。
    #
    # 判定方式的选择（这次踩过坑）：
    #   最初靠 grep 日志里的 "adopting orphaned gateway"。
    #   但 respawn 的 stdout 不进 S99 脚本的日志文件 ——
    #   init 直接 exec 那条命令，没有重定向。于是日志永远
    #   匹配不到，测试误判为失败，而实际领养是成功的
    #   （手工验证确认过）。
    #
    # 改为观测**可验证的外部行为**：新 supervisor 的 STATUS
    # 是否报告 Gateway 在运行，且 pid 与孤儿一致。
    # 这才是运维真正关心的东西，也不依赖日志措辞。
    #
    deadline = time.monotonic() + 25
    adopted = False
    last_status = ""

    while time.monotonic() < deadline:
        rc, out = ssh(f"{ROOT}/bin/service_supervisor status --root {ROOT}",
                      host=host)
        status = strip_ssh_noise(out)

        if "running" in status and "pid=0" not in status:
            #
            # 确认新 supervisor 与旧的是不同进程（旧的值已被 kill -9）。
            #
            new_pids = supervisor_pids(host)

            if new_pids and new_pids[0][0] != pids[0][0]:
                adopted = True
                last_status = status
                break

        last_status = status
        time.sleep(0.5)

    record("新 supervisor 领养了孤儿 Gateway", adopted,
           "" if adopted else last_status.strip()[:120])

    if not adopted:
        return False

    #
    # 领养后仍应能正确报告状态并停止进程。
    # 这两条是关键：领养若只改了内存标志却不会停，
    # 那它就只是个漂亮日志。
    #
    rc, out = ssh(f"{INITD} status", host=host)
    status = strip_ssh_noise(out)
    print(status, flush=True)

    record("领养后 STATUS 报告 Gateway 在运行",
           "running" in status, status.strip()[:60])

    rc, out = ssh(f"{ROOT}/bin/service_supervisor stop --root {ROOT}",
                  host=host)
    out = strip_ssh_noise(out)
    print(out, flush=True)

    record("领养后能停止 Gateway",
           "OK" in out or "stopped" in out, out.strip()[:60])

    # 停止后 pidfile 应被清理
    rc, out = ssh(f"ls {ROOT}/run/service.pid 2>/dev/null || echo GONE",
                  host=host)
    record("停止后 pidfile 已清理", "GONE" in strip_ssh_noise(out))

    return True


def test_no_busy_loop(host):
    """反复崩溃不应导致 init 烧 CPU。

    respawn 的经典风险：进程启动即退的话，init 会不停重拉。
    实测板端有节流（约 1 次/秒），但必须留下证据。
    """
    print("\n" + "=" * 72)
    print("检查五：反复崩溃不会烧满 CPU")
    print("=" * 72)

    rc, out = ssh("awk '{print $14, $15}' /proc/1/stat", host=host)
    before = strip_ssh_noise(out).split()

    if len(before) != 2:
        record("读取 init CPU 计数", False, out[:60])
        return False

    # 连续杀三次，观察 init 的 CPU 时间增量
    for _ in range(3):
        pids = supervisor_pids(host)
        if pids:
            ssh(f"kill -9 {pids[0][0]}", host=host)
        time.sleep(1.5)

    time.sleep(2)

    rc, out = ssh("awk '{print $14, $15}' /proc/1/stat", host=host)
    after = strip_ssh_noise(out).split()

    if len(after) != 2:
        record("读取 init CPU 计数（后）", False, out[:60])
        return False

    delta = (int(after[0]) - int(before[0])) + \
            (int(after[1]) - int(before[1]))

    #
    # 时钟 tick 通常是 10ms。3 次重启若正常，增量应在几到几十 tick。
    # 若陷入忙循环，这个数字会是成千上万。
    #
    record("init CPU 增量在合理范围（未忙循环）",
           delta < 500, f"delta={delta} ticks（3 次重启）")

    # 最终仍应恢复
    pids = supervisor_pids(host)
    record("最后一次崩溃后仍能恢复", bool(pids), f"pids={pids}")

    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default=BOARD)
    args = parser.parse_args()

    print("=" * 72)
    print(f"L12 验收：supervisor 崩溃自愈（G4b）  host={args.host}")
    print("=" * 72)

    rc, out = ssh("uname -r", host=args.host)
    kernel = strip_ssh_noise(out).strip()

    if not re.match(r"^\d+\.\d+", kernel):
        print(f"无法连接板端: {kernel[:100]}", file=sys.stderr)
        return 2

    print(f"板端内核: {kernel}\n")

    try:
        if not test_respawn_entry_present(args.host):
            print("\nrespawn 条目缺失，后续检查无意义。", file=sys.stderr)
            return 1

        test_supervisor_under_init(args.host)
        test_crash_recovery(args.host)
        test_orphan_adoption(args.host)
        test_no_busy_loop(args.host)
    finally:
        # 收尾：恢复到「supervisor 在跑、Gateway 在跑」的可用状态
        ssh(f"{INITD} start", host=args.host)

    total = len(results)
    passed = sum(1 for _, ok, _ in results if ok)

    print("\n" + "=" * 72)
    print(f"合计 {total} 项, 通过 {passed}, 失败 {total - passed}")

    if passed != total:
        print("结论: FAIL")
        for name, ok, detail in results:
            if not ok:
                print(f"  失败: {name} {detail}")
        return 1

    print("结论: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
