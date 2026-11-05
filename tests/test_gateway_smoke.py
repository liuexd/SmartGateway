#!/usr/bin/env python3
"""Exercise the gateway with simulated Bluetooth, WiFi and server peers.

Run from any directory after `make gateway mock_node`:
    python3 tests/test_gateway_smoke.py

Requires Linux, socat, and an available gateway WiFi port (7000). This test
uses a virtual serial pair; it does not validate physical hardware.
"""

import json
import pathlib
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
WIFI_PORT = 7000
TIMEOUT = 10.0


class JsonLines:
    """Retain incomplete and unconsumed TCP lines across reads."""

    def __init__(self, sock):
        self.sock = sock
        self.buffer = b""

    def receive(self, predicate, description):
        deadline = time.monotonic() + TIMEOUT
        while time.monotonic() < deadline:
            while b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                try:
                    message = json.loads(line)
                except (ValueError, UnicodeDecodeError):
                    continue
                if predicate(message):
                    return message
            self.sock.settimeout(max(0.01, min(0.5, deadline - time.monotonic())))
            try:
                chunk = self.sock.recv(8192)
            except socket.timeout:
                continue
            if not chunk:
                raise RuntimeError(f"connection closed while waiting for {description}")
            self.buffer += chunk
        raise RuntimeError(f"timed out waiting for {description}")


def send_json(sock, message):
    sock.sendall((json.dumps(message, separators=(",", ":")) + "\n").encode())


def listen(port=0):
    sock = socket.socket()
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind(("127.0.0.1", port))
        sock.listen()
        sock.settimeout(TIMEOUT)
        return sock
    except Exception:
        sock.close()
        raise


def stop(process, signum=signal.SIGTERM):
    if process.poll() is None:
        process.send_signal(signum)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise RuntimeError(f"process required forced termination: {process.args}")


def main():
    if not shutil.which("socat"):
        raise RuntimeError("socat is required")
    for binary in ("smart_gateway", "mock_node"):
        if not (ROOT / "build" / binary).is_file():
            raise RuntimeError(f"build/{binary} is missing; run make gateway mock_node")
    # Probe before starting anything; do not disturb an existing gateway.
    try:
        with listen(WIFI_PORT):
            pass
    except OSError as exc:
        raise RuntimeError(f"gateway WiFi port {WIFI_PORT} is unavailable: {exc}") from exc

    processes = []
    sockets = []
    cleanup_errors = []
    with tempfile.TemporaryDirectory(prefix="smartgateway-smoke-") as directory:
        directory = pathlib.Path(directory)
        log_path = directory / "gateway.log"
        with log_path.open("w+", encoding="utf-8") as log:
            def start(*args):
                process = subprocess.Popen(args, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
                processes.append(process)
                return process

            try:
                # Keep an unused port reserved so that connection attempts fail predictably.
                unavailable = socket.socket()
                unavailable.bind(("127.0.0.1", 0))
                sockets.append(unavailable)
                port = unavailable.getsockname()[1]
                gateway = start("./build/smart_gateway", "--no-serial", "--server", "127.0.0.1", "--port", str(port))
                time.sleep(1.5)
                if gateway.poll() is not None:
                    raise RuntimeError("no-serial gateway exited during startup")
                stop(gateway, signal.SIGINT)
                if gateway.returncode != 0:
                    raise RuntimeError(f"no-serial gateway exited with {gateway.returncode}")
                print("PASS no-serial startup and SIGINT", flush=True)

                serial_gateway = directory / "ttyGW"
                serial_node = directory / "ttyNODE"
                serial_bridge = start("socat", f"pty,raw,echo=0,link={serial_gateway}",
                                      f"pty,raw,echo=0,link={serial_node}")
                deadline = time.monotonic() + TIMEOUT
                while not (serial_gateway.exists() and serial_node.exists()):
                    if serial_bridge.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError("virtual serial pair did not start")
                    time.sleep(0.05)

                server = listen()
                sockets.append(server)
                server_port = server.getsockname()[1]
                gateway = start("./build/smart_gateway", "--serial", str(serial_gateway), "--baud", "9600",
                                "--server", "127.0.0.1", "--port", str(server_port))
                upstream, _ = server.accept()
                sockets.append(upstream)
                server_lines = JsonLines(upstream)
                start("./build/mock_node", str(serial_node))
                server_lines.receive(lambda m: m.get("node") == "NODE01" and m.get("type") == "DATA", "Bluetooth DATA")
                print("PASS virtual Bluetooth DATA", flush=True)

                wifi = socket.create_connection(("127.0.0.1", WIFI_PORT), timeout=TIMEOUT)
                sockets.append(wifi)
                wifi_lines = JsonLines(wifi)
                for message in (
                    {"node": "NODE02", "seq": 1, "type": "DATA", "fields": {"LIGHT_RAW": "2200"}},
                    {"node": "NODE02", "seq": 2, "type": "ACK", "fields": {"LED": "1"}},
                    {"node": "NODE02", "seq": 3, "type": "NACK", "error": "bad_cmd"},
                ):
                    send_json(wifi, message)
                    server_lines.receive(lambda m: m.get("node") == "NODE02" and m.get("seq") == message["seq"]
                                         and m.get("type") == message["type"], f"WiFi {message['type']}")
                print("PASS WiFi DATA, ACK and NACK", flush=True)

                command = {"node": "NODE02", "sender": "GATEWAY", "seq": 5, "type": "CMD", "fields": {"LED": "1"}}
                send_json(upstream, command)
                wifi_lines.receive(lambda m: m.get("type") == "CMD" and m.get("seq") == 5, "WiFi command")
                print("PASS WiFi command delivery", flush=True)
                command.update(node="NODE01", seq=6)
                send_json(upstream, command)
                server_lines.receive(lambda m: m.get("node") == "NODE01" and m.get("type") == "ACK"
                                     and m.get("seq") == 6, "Bluetooth command ACK")
                print("PASS Bluetooth command and ACK", flush=True)

                wifi.close()
                upstream.close()
                server.close()
                # Leave the server down long enough for the gateway to observe disconnection.
                time.sleep(1.5)
                server = listen(server_port)
                sockets.append(server)
                upstream, _ = server.accept()
                sockets.append(upstream)
                JsonLines(upstream).receive(lambda m: m.get("node") == "NODE01" and m.get("type") == "DATA",
                                            "DATA after reconnect")
                print("PASS northbound reconnect and resumed DATA", flush=True)
                stop(gateway, signal.SIGINT)
                if gateway.returncode != 0:
                    raise RuntimeError(f"serial gateway exited with {gateway.returncode}")
                print("PASS serial-mode SIGINT shutdown", flush=True)
            except Exception:
                log.flush()
                log.seek(0)
                print("--- gateway and simulated peer log ---", file=sys.stderr)
                print(log.read(), file=sys.stderr)
                raise
            finally:
                for sock in reversed(sockets):
                    sock.close()
                for process in reversed(processes):
                    if process.poll() is None:
                        try:
                            stop(process)
                        except RuntimeError as exc:
                            cleanup_errors.append(str(exc))
    if cleanup_errors:
        raise RuntimeError("; ".join(cleanup_errors))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
