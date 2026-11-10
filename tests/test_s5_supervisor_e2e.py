#!/usr/bin/env python3
"""S5 端到端测试：Service Supervisor 与 IPC

测什么
------
不是在测协议编解码（那由 `make test-ipc` 覆盖），而是测
**真实进程生命周期**和它要解决的 L3/L6 两个问题：

  1. supervisor 能否启动/停止 Gateway，并维护正确的 pidfile
  2. STATUS 是否与真实进程一致（尤其是进程自己死掉之后）
  3. 陈旧 socket 能否自动恢复（supervisor 被 kill -9 之后）
  4. 重复 start 是否幂等（ALREADY，而不是启动第二个实例）
  5. 停止一个忽略 SIGTERM 的进程时是否升级为 SIGKILL

为什么这些必须真实跑
--------------------
L6（TOCTOU）的修复依据是"未回收的子进程 pid 不会被复用"。
这个性质只有对**真实父子关系**才成立，任何 mock 都无法验证。

用法:
  python3 tests/test_s5_supervisor_e2e.py
  python3 tests/test_s5_supervisor_e2e.py --keep
"""

import argparse
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SUP = ROOT / "bsp/apps/service_supervisor/service_supervisor"
GATEWAY = ROOT / "build/smart_gateway"
WORK = ROOT / "build/s5-test"

results = []
procs = []

#
# Gateway 自己监听 7000（WiFi 服务端口），配置里的 port 是它
# **连接**的北向服务器地址。两者不是一回事，最初我把它们搞混了：
# 断言"19998 就绪"永远失败，因为根本没有人在 19998 上 listen。
#
GW_PORT = 7000


def record(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}"
          + (f": {detail}" if detail else ""), flush=True)


def listen_port(port, timeout=0.3):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout):
            return True
    except ConnectionRefusedError:
        return False
    except OSError:
        return True


def read_stat(pid):
    try:
        data = Path(f"/proc/{pid}/stat").read_text()
    except OSError:
        return None, None
    rp = data.rfind(")")
    if rp < 0:
        return None, None
    f = data[rp + 2:].split()
    if len(f) < 20:
        return None, None
    return f[0], int(f[19])


def setup(name="e2e"):
    work = WORK / name
    if work.exists():
        shutil.rmtree(work)

    (work / "run").mkdir(parents=True)

    # 安装目录结构：current -> releases/v0.1.0
    rel = work / "releases" / "v0.1.0"
    rel.mkdir(parents=True)

    gw = rel / "smart_gateway"
    shutil.copy2(GATEWAY, gw)
    gw.chmod(0o755)

    cur = work / "current"
    cur.symlink_to("releases/v0.1.0")

    conf = work / "service.conf"
    conf.write_text(
        f"executable={work}/current/smart_gateway\n"
        f"serial=\n"          # 空 -> --no-serial
        f"server=127.0.0.1\n"
        f"port=19998\n"
        f"stop_timeout_ms=3000\n")

    return work


def sup_call(work, cmd, timeout=30):
    return subprocess.run(
        [str(SUP), cmd, "--root", str(work)],
        capture_output=True, text=True, timeout=timeout)


def wait_port(port, expect, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if listen_port(port) == expect:
            return True
        time.sleep(0.05)
    return False


def write_pidfile(work, pid):
    st = read_stat(pid)[1]
    if st is None:
        return False
    p = work / "run" / "service.pid"
    p.write_text(f"{pid} {st}\n")
    p.chmod(0o600)
    return True


def procs_under(path):
    base = str(Path(path).resolve())
    found = []
    for e in Path("/proc").iterdir():
        if not e.name.isdigit():
            continue
        try:
            exe = os.readlink(e / "exe").replace(" (deleted)", "")
        except OSError:
            continue
        if exe.startswith(base):
            found.append(int(e.name))
    return found


def cleanup():
    for p in procs:
        try:
            p.terminate()
            p.wait(timeout=3)
        except Exception:
            try:
                p.kill()
            except Exception:
                pass
    procs.clear()

    for pid in procs_under(WORK):
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass

    time.sleep(0.2)


# ---------------------------------------------------------------------

def test_ipc_absent(work):
    """supervisor 未运行时，CLI 必须给出可区分的原因。"""
    print("\n" + "=" * 72)
    print("用例一：supervisor 未运行时的错误提示")
    print("=" * 72)

    r = sup_call(work, "status", timeout=10)
    out = r.stdout + r.stderr
    print(out, flush=True)

    record("未运行时返回失败", r.returncode != 0, f"exit={r.returncode}")

    #
    # 必须明确说"socket 不存在"，而不是笼统的"连接失败"。
    #
    # ENOENT   -> supervisor 没装/没启动
    # ECONNREFUSED -> 装了但挂了
    # 运维的处置方式完全不同，这两句不能混。
    #
    record("指出 socket 不存在（而非笼统的连接失败）",
           "does not exist" in out or "not running" in out)


def test_lifecycle(work):
    """start -> status -> 幂等 start -> stop -> status。"""
    print("\n" + "=" * 72)
    print("用例二：完整生命周期")
    print("=" * 72)

    sup = subprocess.Popen(
        [str(SUP), "run", "--root", str(work)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    procs.append(sup)

    sock = work / "run" / "supervisor.sock"
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline and not sock.exists():
        time.sleep(0.05)

    record("supervisor 已启动并创建 socket", sock.exists())

    if not sock.exists():
        return

    # STATUS：初始应为未运行
    r = sup_call(work, "status")
    record("初始状态为 stopped", "stopped" in (r.stdout + r.stderr),
           (r.stdout + r.stderr).strip()[:60])

    # START
    r = sup_call(work, "start")
    out = r.stdout + r.stderr
    print(out, flush=True)

    record("start 成功", r.returncode == 0 and "OK" in out,
           f"exit={r.returncode}")

    record("Gateway 端口已就绪", wait_port(GW_PORT, True, 8))

    pid = procs_under(work)
    record("Gateway 进程存在", len(pid) == 1, f"pids={pid}")

    # pidfile 必须与真实进程一致
    pf = (work / "run" / "service.pid").read_text().split()
    record("pidfile 已写入", len(pf) == 2, " ".join(pf)[:40])

    if len(pf) == 2 and pid:
        record("pidfile 的 pid 与实际进程一致",
               int(pf[0]) == pid[0],
               f"pidfile={pf[0]} actual={pid[0]}")

    # 幂等 start
    r = sup_call(work, "start")
    out = r.stdout + r.stderr
    print(out, flush=True)

    record("重复 start 返回幂等（ALREADY 或 OK）",
           "ALREADY" in out or "OK" in out, out.strip()[:60])
    record("重复 start 未产生第二个实例",
           len(procs_under(work)) == 1,
           f"count={len(procs_under(work))}")

    # STOP
    r = sup_call(work, "stop")
    out = r.stdout + r.stderr
    print(out, flush=True)

    record("stop 成功", r.returncode == 0, f"exit={r.returncode}")
    record("Gateway 已退出", len(procs_under(work)) == 0,
           f"pids={procs_under(work)}")
    record("端口已释放", wait_port(GW_PORT, False, 5))
    record("pidfile 已清理", not (work / "run" / "service.pid").exists())

    # 重复 stop 不应报错（init 的 shutdown 路径会重复调用）
    r = sup_call(work, "stop")
    record("重复 stop 不报失败", r.returncode == 0,
           f"exit={r.returncode}")


def test_crash_detection(work):
    """Gateway 自己死掉后，STATUS 必须如实反映。"""
    print("\n" + "=" * 72)
    print("用例三：Gateway 崩溃后的状态一致性")
    print("=" * 72)

    r = sup_call(work, "start")
    record("重新启动成功", r.returncode == 0)

    wait_port(GW_PORT, True, 8)
    pids = procs_under(work)
    record("Gateway 在运行", len(pids) == 1)

    if not pids:
        return

    # 外部 kill -9 模拟崩溃
    os.kill(pids[0], signal.SIGKILL)

    # 给 supervisor 的轮询一点时间回收
    time.sleep(1.0)

    r = sup_call(work, "status")
    out = r.stdout + r.stderr
    print(out, flush=True)

    record("崩溃后 STATUS 报告 stopped",
           "stopped" in out,
           out.strip()[:60])

    #
    # pidfile 必须被清理。
    #
    # 若残留，下次 supervisor 启动会误判"有个不是我的进程在跑"
    # 而拒绝启动——一个已死的进程把服务永久卡住。
    #
    record("崩溃后 pidfile 已清理",
           not (work / "run" / "service.pid").exists())

    # 必须能重新启动
    r = sup_call(work, "start")
    record("崩溃后可重新启动", r.returncode == 0,
           (r.stdout + r.stderr).strip()[:60])

    sup_call(work, "stop")


def test_sigterm_ignored(work):
    """Gateway 忽略 SIGTERM 时，supervisor 必须升级为 SIGKILL。"""
    print("\n" + "=" * 72)
    print("用例四：忽略 SIGTERM -> SIGKILL")
    print("=" * 72)

    # 换成顽固 Gateway
    src = WORK / "stubborn.c"
    src.write_text(
        "#include <signal.h>\n"
        "#include <unistd.h>\n"
        "int main(void){signal(SIGTERM,SIG_IGN);"
        "for(;;)pause();return 0;}\n")

    binpath = WORK / "stubborn"
    r = subprocess.run(
        ["gcc", "-O1", "-o", str(binpath), str(src)],
        capture_output=True)

    record("构造忽略 SIGTERM 的进程", r.returncode == 0)

    if r.returncode != 0:
        return

    # 直接替换 current 指向的二进制
    gw = work / "current" / "smart_gateway"
    target = Path(os.path.realpath(gw))
    target.unlink()
    shutil.copy2(binpath, target)
    target.chmod(0o755)

    r = sup_call(work, "start")
    record("顽固进程启动成功", r.returncode == 0,
           (r.stdout + r.stderr).strip()[:60])

    time.sleep(0.5)

    pids = procs_under(work)
    record("顽固进程在运行", len(pids) == 1, f"pids={pids}")

    started = time.monotonic()
    r = sup_call(work, "stop", timeout=30)
    elapsed = time.monotonic() - started

    out = r.stdout + r.stderr
    print(out, flush=True)

    record("stop 最终成功", r.returncode == 0, f"exit={r.returncode}")

    #
    # 关键：必须真的等到进程消失，而不是"发完信号就返回成功"。
    #
    alive = len(procs_under(work))
    record("顽固进程确实已终止", alive == 0, f"remaining={alive}")

    #
    # 耗时应明显超过 stop_timeout（3s），说明确实走了
    # SIGTERM 等待 + SIGKILL 的路径。
    #
    record("耗时包含 SIGTERM 宽限期",
           elapsed >= 2.5, f"elapsed={elapsed:.2f}s")

    record("报告使用 SIGKILL", "SIGKILL" in out, out.strip()[:60])


def test_stale_socket(work):
    """supervisor 被 kill -9 后，陈旧 socket 应能自动恢复。"""
    print("\n" + "=" * 72)
    print("用例五：supervisor 崩溃后的陈旧 socket")
    print("=" * 72)

    sup = subprocess.Popen(
        [str(SUP), "run", "--root", str(work)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    procs.append(sup)

    sock = work / "run" / "supervisor.sock"
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline and not sock.exists():
        time.sleep(0.05)

    record("supervisor 启动", sock.exists())

    # kill -9：socket 文件会残留
    sup.kill()
    sup.wait(timeout=5)
    procs.remove(sup)

    record("崩溃后 socket 文件残留", sock.exists())

    # 手工清掉可能残留的 Gateway（父进程已死，它会被 init 接管）
    for pid in procs_under(work):
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass
    time.sleep(0.3)

    # 清理 pidfile（否则新 supervisor 会因"有进程不是我的"而拒绝）
    pf = work / "run" / "service.pid"
    if pf.exists():
        pf.unlink()

    # 重启：必须能自动清理陈旧 socket
    sup2 = subprocess.Popen(
        [str(SUP), "run", "--root", str(work)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    procs.append(sup2)

    time.sleep(1.0)

    r = sup_call(work, "status", timeout=10)
    out = r.stdout + r.stderr
    print(out, flush=True)

    record("陈旧 socket 自动恢复，supervisor 可用",
           r.returncode == 0 and "stopped" in out,
           out.strip()[:60])

    sup2.terminate()
    try:
        sup2.wait(timeout=5)
    except subprocess.TimeoutExpired:
        sup2.kill()
    procs.remove(sup2)


def test_second_supervisor_rejected(work):
    """第二个 supervisor 必须拒绝启动（否则会各起一个 Gateway）。"""
    print("\n" + "=" * 72)
    print("用例六：重复 supervisor 被拒绝")
    print("=" * 72)

    sup = subprocess.Popen(
        [str(SUP), "run", "--root", str(work)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    procs.append(sup)

    sock = work / "run" / "supervisor.sock"
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline and not sock.exists():
        time.sleep(0.05)

    r = subprocess.run(
        [str(SUP), "run", "--root", str(work)],
        capture_output=True, text=True, timeout=10)

    out = r.stdout + r.stderr
    print(out, flush=True)

    record("第二个 supervisor 被拒绝",
           r.returncode != 0, f"exit={r.returncode}")
    record("报告已有实例在运行",
           "already running" in out.lower(),
           out.strip()[:80])

    sup.terminate()
    try:
        sup.wait(timeout=5)
    except subprocess.TimeoutExpired:
        sup.kill()
    procs.remove(sup)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--keep", action="store_true",
                        help="保留工作目录便于排查")
    args = parser.parse_args()

    print("=" * 72)
    print("S5 端到端：Service Supervisor 与 IPC")
    print("=" * 72)

    for path, label in [(SUP, "service_supervisor"),
                        (GATEWAY, "smart_gateway")]:
        if not path.exists():
            print(f"缺少 {label}: {path}", file=sys.stderr)
            print("  先运行: make supervisor && make gateway", file=sys.stderr)
            return 2

    work = setup()

    try:
        test_ipc_absent(work)
        test_lifecycle(work)
        test_crash_detection(work)
        test_sigterm_ignored(work)
        test_stale_socket(work)
        test_second_supervisor_rejected(work)
    finally:
        cleanup()

        if not args.keep and WORK.exists():
            shutil.rmtree(WORK, ignore_errors=True)

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
