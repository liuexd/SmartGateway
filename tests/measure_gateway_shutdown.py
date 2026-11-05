#!/usr/bin/env python3
"""Gateway 退出延迟测量.

测量 SIGTERM 到主进程真正退出所需时间, 用于对比优化前后。

用法:
  python3 tests/measure_gateway_shutdown.py --bin ./build/smart_gateway
  python3 tests/measure_gateway_shutdown.py --bin ./build/smart_gateway \
      --server 192.0.2.1 --port 9000
"""

import argparse
import os
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

HOST = "127.0.0.1"
GW_PORT = 7000


def port_free(port):
    import socket

    sock = socket.socket()
    try:
        sock.bind((HOST, port))
        return True
    except OSError:
        return False
    finally:
        sock.close()


def measure(binary, server, port, settle=3.0, kill_after=60.0):
    """启动 Gateway, 等待 settle 秒后发 SIGTERM, 返回退出耗时。"""
    if not port_free(GW_PORT):
        return {"error": f"TCP {GW_PORT} occupied"}

    out_path = Path(f"/tmp/gw-shutdown-{os.getpid()}.log")

    with out_path.open("wb") as log:
        proc = subprocess.Popen(
            [
                binary,
                "--no-serial",
                "--server",
                server,
                "--port",
                str(port),
            ],
            stdout=log,
            stderr=subprocess.STDOUT,
        )

        # 让 Gateway 进入稳定状态(可能已进入阻塞 connect)
        time.sleep(settle)

        if proc.poll() is not None:
            return {"error": f"Gateway exited early rc={proc.returncode}"}

        # 确认它确实处于 connect 中/运行中
        alive_before = proc.poll() is None

        term_at = time.monotonic()
        proc.send_signal(signal.SIGTERM)

        forced = False
        try:
            proc.wait(timeout=kill_after)
        except subprocess.TimeoutExpired:
            forced = True
            proc.kill()
            proc.wait()

        elapsed = time.monotonic() - term_at

    text = out_path.read_text(errors="replace")
    out_path.unlink(missing_ok=True)

    return {
        "elapsed": elapsed,
        "alive_before": alive_before,
        "forced_kill": forced,
        "rc": proc.returncode,
        "log": text,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", default="./build/smart_gateway")
    parser.add_argument("--server", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=45999)
    parser.add_argument("--settle", type=float, default=3.0)
    parser.add_argument("--repeat", type=int, default=1)
    args = parser.parse_args()

    label = f"{args.server}:{args.port}"
    print(f"binary   : {args.bin}")
    print(f"upstream : {label}")
    print(f"settle   : {args.settle}s")
    print("-" * 60)

    samples = []

    for i in range(args.repeat):
        result = measure(
            args.bin, args.server, args.port, args.settle
        )

        if "error" in result:
            print(f"run {i + 1}: ERROR {result['error']}")
            return 2

        samples.append(result["elapsed"])
        print(
            f"run {i + 1}: shutdown={result['elapsed']:.3f}s "
            f"rc={result['rc']} forced_kill={result['forced_kill']}"
        )

        if result["forced_kill"]:
            print("  WARNING: 需要 SIGKILL 才退出")

        for line in result["log"].splitlines():
            if "tcp_connect failed" in line or "SERVER WORKER" in line:
                pass

    worst = max(samples)
    print("-" * 60)
    print(f"max shutdown latency: {worst:.3f}s")

    return 0


if __name__ == "__main__":
    sys.exit(main())
