#!/usr/bin/env python3
"""tcp_client 有界 connect 与可中断连接单元测试.

覆盖:
  1. 成功连接  -> 返回 fd, socket 恢复阻塞模式
  2. 连接被拒  -> ECONNREFUSED, 立即返回
  3. 黑洞地址  -> ETIMEDOUT, 在 timeout 附近返回(而非内核 18~21s)
  4. 停止标志  -> 连接中途置 0 立即返回 EINTR
  5. 失败路径  -> 不泄漏 fd
"""

import errno
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build", "tests")
SRC = os.path.join(ROOT, "tests", "test_tcp_connect_bounded.c")

results = []


def record(name, ok, detail):
    results.append((name, bool(ok), detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)


def build():
    os.makedirs(BUILD, exist_ok=True)
    out = os.path.join(BUILD, "test_tcp_connect_bounded")

    cmd = [
        "gcc",
        "-I" + os.path.join(ROOT, "gateway"),
        "-Wall", "-Wextra", "-Werror",
        "-std=c11", "-pthread",
        SRC,
        os.path.join(ROOT, "gateway", "tcp_client.c"),
        "-o", out,
    ]

    proc = subprocess.run(cmd, capture_output=True, text=True)

    if proc.returncode != 0:
        print(proc.stdout, proc.stderr)
        raise SystemExit("build failed")

    return out


def count_fds():
    return len(os.listdir("/proc/self/fd"))


def run_case(binary, *args):
    env = dict(os.environ)
    proc = subprocess.run(
        [binary, *args],
        capture_output=True,
        text=True,
        env=env,
        timeout=120,
    )
    return proc


def main():
    binary = build()

    # ---- 1. 成功连接 ----
    with socket.socket() as srv:
        srv.bind(("127.0.0.1", 0))
        srv.listen(4)
        port = srv.getsockname()[1]

        proc = run_case(binary, "success", "127.0.0.1", str(port))
        record(
            "成功连接",
            proc.returncode == 0
            and "rc=0 fd_ok=1 blocking=1" in proc.stdout,
            (proc.stdout + proc.stderr).strip().splitlines()[-1],
        )

    # ---- 2. 连接被拒 ----
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    dead_port = probe.getsockname()[1]
    probe.close()

    proc = run_case(binary, "refused", "127.0.0.1", str(dead_port))
    record(
        "连接被拒返回 ECONNREFUSED",
        proc.returncode == 0 and "errno=111" in proc.stdout,
        (proc.stdout + proc.stderr).strip().splitlines()[-1],
    )

    # ---- 3. 黑洞地址：有界超时 ----
    for timeout_ms in (800, 1500):
        started = time.monotonic()
        proc = run_case(
            binary, "bounded", "192.0.2.1", "9000", str(timeout_ms)
        )
        elapsed_ms = (time.monotonic() - started) * 1000

        # 允许 400ms 的调度/进程启动误差
        upper = timeout_ms + 400

        record(
            f"黑洞地址 {timeout_ms}ms 有界超时",
            proc.returncode == 0
            and "errno=110" in proc.stdout
            and elapsed_ms < upper,
            f"elapsed={elapsed_ms:.0f}ms, upper={upper}ms, "
            f"{(proc.stdout + proc.stderr).strip().splitlines()[-1]}",
        )

    # ---- 4. 停止标志中断 ----
    started = time.monotonic()
    proc = run_case(binary, "cancel", "192.0.2.1", "9000", "5000")
    elapsed_ms = (time.monotonic() - started) * 1000

    record(
        "停止标志中断 connect (EINTR)",
        proc.returncode == 0
        and "errno=4" in proc.stdout
        and elapsed_ms < 1200,
        f"elapsed={elapsed_ms:.0f}ms (超时设为 5000ms, "
        f"应在置标志后 ~200ms 内返回)",
    )

    # ---- 5. fd 不泄漏 ----
    proc = run_case(binary, "fdleak", "127.0.0.1", str(dead_port))
    record(
        "失败路径不泄漏 fd",
        proc.returncode == 0 and "leak=0" in proc.stdout,
        (proc.stdout + proc.stderr).strip().splitlines()[-1],
    )

    failed = [r for r in results if not r[1]]

    print()
    print(f"合计 {len(results)} 项, "
          f"通过 {len(results) - len(failed)}, 失败 {len(failed)}")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
