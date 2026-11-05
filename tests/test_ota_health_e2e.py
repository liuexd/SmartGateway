
#!/usr/bin/env python3
"""SmartGateway OTA L2 business-health integration test."""

import os
import json
import secrets
import socket
import subprocess
import threading
import time
from pathlib import Path

HOST = "127.0.0.1"
GW_PORT = 7000
SERVER_PORT = 9000
LOG = Path("build/ota-test/ota_health_e2e.log")
GATEWAY_BIN = os.environ.get(
    "OTA_GATEWAY_BIN",
    "./build/smart_gateway"
)

NODE = "OTA01"
SEQ = secrets.randbelow(1_000_000_000) + 1
FIELDS = {"T": "253", "H": "601"}

connected = threading.Event()
finished = threading.Event()
result = {"ok": False}


def upstream_receiver(server):
    try:
        server.settimeout(8)
        conn, _ = server.accept()
        connected.set()

        with conn:
            conn.settimeout(0.5)
            buffer = b""
            deadline = time.monotonic() + 5

            while time.monotonic() < deadline:
                try:
                    chunk = conn.recv(4096)
                except socket.timeout:
                    continue

                if not chunk:
                    break

                buffer += chunk

                while b"\n" in buffer:
                    line, buffer = buffer.split(b"\n", 1)

                    try:
                        msg = json.loads(line)
                    except (ValueError, UnicodeDecodeError):
                        continue

                    if (
                        msg.get("node") == NODE
                        and msg.get("seq") == SEQ
                        and msg.get("type") == "DATA"
                        and msg.get("fields") == FIELDS
                    ):
                        result["ok"] = True
                        return

    except OSError as exc:
        result["error"] = str(exc)
    finally:
        finished.set()


def wait_gateway_ready(proc, timeout=3):
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError("Gateway exited before TCP ready")

        try:
            with socket.create_connection(
                (HOST, GW_PORT), timeout=0.2
            ):
                return
        except OSError:
            time.sleep(0.05)

    raise TimeoutError("Gateway TCP 7000 not ready")


def main():
    LOG.parent.mkdir(parents=True, exist_ok=True)

    # 防止误用已有的 Gateway 服务
    try:
        with socket.create_connection(
            (HOST, GW_PORT), timeout=0.2
        ):
            raise RuntimeError("TCP 7000 is already occupied")
    except ConnectionRefusedError:
        pass

    with socket.socket() as server:
        # 端口 0：请求操作系统分配可用端口
        server.bind((HOST, 0))
        server.listen(1)

        # 获取实际分配的 TCP 端口
        server_port = server.getsockname()[1]

        print(f"Test upstream port: {server_port}")

        receiver = threading.Thread(
            target=upstream_receiver,
            args=(server,),
            daemon=True,
        )
        receiver.start()

        with LOG.open("w") as output:
            proc = subprocess.Popen(
                [
                    GATEWAY_BIN,
                    "--no-serial",
                    "--server", HOST,
                    "--port", str(server_port),
                ],
                stdout=output,
                stderr=subprocess.STDOUT,
            )

            success = False

            try:
                print(f"Gateway PID: {proc.pid}")

                wait_gateway_ready(proc)

                if not connected.wait(3):
                    raise TimeoutError("Gateway upstream not connected")

                probe = {
                    "node": NODE,
                    "seq": SEQ,
                    "type": "DATA",
                    "fields": FIELDS,
                }

                payload = (
                    json.dumps(probe, separators=(",", ":")) + "\n"
                ).encode()

                with socket.create_connection(
                    (HOST, GW_PORT), timeout=2
                ) as client:
                    client.sendall(payload)

                print(f"Probe sent: node={NODE}, seq={SEQ}")

                if not finished.wait(6):
                    raise TimeoutError("Upstream response timeout")

                if not result["ok"]:
                    raise RuntimeError(
                        result.get("error", "Probe not forwarded")
                    )

                success = True

            except (OSError, RuntimeError, TimeoutError) as exc:
                print(f"FAIL: {exc}")

            finally:
                if proc.poll() is None:
                    proc.terminate()

                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()

                print(f"Gateway exit code: {proc.returncode}")

        receiver.join(timeout=1)

    if success and proc.returncode == 0:
        print("PASS: OTA L2 business health")
        return 0

    print(f"FAIL: see Gateway log: {LOG}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
