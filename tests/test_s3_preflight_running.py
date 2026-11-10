#!/usr/bin/env python3
"""S3 前置检查验收：Gateway 正在运行时执行升级.

D23 的测试形态假设升级开始前 Gateway 没在运行，
所以"停止旧版 → 确认退出 → 切换版本"这段逻辑从未被验证过。

本脚本补上这个场景：
  1. 先在 7000 端口启动一个 Gateway（真实进程，非模拟）；
  2. 用 supervisor 写入对应的 pidfile，让它"看起来是正式部署中的实例"；
  3. 执行升级事务；
  4. 检查：
     - 升级流程确实检测到 RUNNING 并停掉了它；
     - 停不掉时必须中止，current 不得改变。

用法:
  python3 tests/test_s3_preflight_running.py
"""

import json
import os
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AGENT = ROOT / "bsp/apps/ota_agent/ota_agent_x86"
GW = ROOT / "build/smart_gateway"
WORK = ROOT / "build/ota-test/s3-preflight"

HOST = "127.0.0.1"
GW_PORT = 7000
OLD_VERSION = "0.1.0-test1"
NEW_VERSION = "0.1.1-test1"

results = []


def record(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}"
          + (f": {detail}" if detail else ""), flush=True)


def port_listening(port, timeout=0.3):
    try:
        with socket.create_connection((HOST, port), timeout=timeout):
            return True
    except ConnectionRefusedError:
        return False
    except OSError:
        return True


def wait_port(port, expect, seconds=5.0):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if port_listening(port) == expect:
            return True
        time.sleep(0.05)
    return False


def gateway_procs():
    """用 /proc/<pid>/exe 精确识别 Gateway 进程。"""
    found = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            exe = os.readlink(entry / "exe")
        except OSError:
            continue
        if exe.endswith("/smart_gateway") or \
           exe.endswith("/smart_gateway (deleted)"):
            found.append(int(entry.name))
    return found


def read_starttime(pid):
    """从 /proc/<pid>/stat 取 starttime（从最后一个 ')' 之后解析）。"""
    try:
        data = Path(f"/proc/{pid}/stat").read_text()
    except OSError:
        return None
    rp = data.rfind(")")
    if rp < 0:
        return None
    fields = data[rp + 2:].split()
    # ')' 之后第一个是 state(第3字段)，starttime 是第22字段 -> 索引 19
    if len(fields) < 20:
        return None
    return int(fields[19])


def write_pidfile(install, pid):
    """按 supervisor 的格式写 pidfile（含 boot_id）。"""
    run_dir = install / "run"
    run_dir.mkdir(parents=True, exist_ok=True)

    try:
        boot_id = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
    except OSError:
        boot_id = ""

    starttime = read_starttime(pid)
    if starttime is None:
        return False

    content = f"{pid} {starttime}"
    if boot_id:
        content += f" {boot_id}"
    content += "\n"

    (run_dir / "gateway.pid").write_text(content)
    (run_dir / "gateway.pid").chmod(0o600)
    return True


def read_pidfile(install):
    path = install / "run" / "gateway.pid"
    if not path.exists():
        return None
    return path.read_text().strip()


def setup_install():
    """从已构建好的环境复制一份独立安装目录。"""
    src = ROOT / "build/ota-test/install-x86-fault.2e0Csr"

    if not src.exists():
        print("需要先运行: sh tests/setup_ota_test_env.sh", file=sys.stderr)
        return None

    if WORK.exists():
        shutil.rmtree(WORK)

    # symlinks=True：保留 current 软链本身，
    # 否则 copytree 会把它解引用成真实目录，
    # 后续 unlink/symlink 就会失败。
    shutil.copytree(src, WORK, symlinks=True)

    # 清掉可能残留的 run 目录
    if (WORK / "run").exists():
        shutil.rmtree(WORK / "run")

    # 重置为 CONFIRMED/旧版
    init = ROOT / "build/ota-test/ota_baseline_init"
    subprocess.run([str(init), str(WORK / "state")],
                   check=True, capture_output=True)

    current = WORK / "current"
    if current.is_symlink() or current.is_file():
        current.unlink()
    elif current.is_dir():
        shutil.rmtree(current)

    current.symlink_to(f"releases/v{OLD_VERSION}")

    return WORK


def cleanup(proc=None):
    if proc is not None:
        try:
            proc.terminate()
            proc.wait(timeout=3)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass

    # 兜底：杀掉任何遗留的 Gateway
    for pid in gateway_procs():
        try:
            os.kill(pid, 15)
        except OSError:
            pass
    time.sleep(0.5)

    if WORK.exists():
        shutil.rmtree(WORK, ignore_errors=True)


def scenario_running_gateway():
    """场景一：Gateway 正在运行时发起升级，应被正确停止。"""
    print("\n" + "=" * 72)
    print("场景一：Gateway 正在运行 -> 升级前应先停止它")
    print("=" * 72)

    install = setup_install()
    if install is None:
        return False

    if port_listening(GW_PORT):
        record("前置：7000 空闲", False, "端口已被占用")
        return False

    record("前置：7000 空闲", True)

    # 启动真实 Gateway
    gw_bin = install / "current" / "smart_gateway"
    proc = subprocess.Popen(
        [str(gw_bin), "--no-serial",
         "--server", HOST, "--port", "19999"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )

    ok = wait_port(GW_PORT, True)
    record("Gateway 已启动并监听 7000", ok)

    if not ok:
        cleanup(proc)
        return False

    # 写 pidfile，让 supervisor 认为它是"正式部署中的实例"
    written = write_pidfile(install, proc.pid)
    record("已写入 pidfile（含 boot_id）", written,
           read_pidfile(install) or "")

    if not written:
        cleanup(proc)
        return False

    # 验证 supervisor 能查到 RUNNING
    q = subprocess.run(
        [str(AGENT), "status", str(install)],
        capture_output=True, text=True,
    )
    record("supervisor status 可用", q.returncode == 0,
           (q.stdout or q.stderr).strip().split("\n")[0])

    # 执行升级（会走前置检查，停掉正在运行的 Gateway）
    print("\n--- 执行升级事务 ---", flush=True)

    conv = subprocess.run(
        [str(AGENT), "upgrade-test",
         str(ROOT / f"build/ota-test/real-x86/v{NEW_VERSION}"),
         str(install)],
        capture_output=True, text=True, timeout=180,
    )

    out = conv.stdout + conv.stderr
    print(out, flush=True)

    # 关键断言：前置检查识别出 RUNNING 并尝试停止
    detected = "Pre-upgrade gateway state: RUNNING" in out
    record("检测到运行中的 Gateway", detected)

    stopped = ("NOTICE: stopping running gateway" in out or
               "PASS: gateway stopped" in out)
    record("执行了停止动作", stopped)

    # 旧进程必须真的退出
    time.sleep(0.5)
    still = proc.poll() is None
    record("原 Gateway 进程已退出", not still,
           f"pid={proc.pid}")

    # 本场景**没有**注入北向故障（D23 才注入），
    # 因此升级应当成功完成，而不是回滚。
    # 这里要验证的是"前置检查能正确处理运行中的实例"，
    # 不是回滚逻辑本身（那由 D23 覆盖）。
    state_file = install / "state" / "ota_state.json"
    state = json.loads(state_file.read_text()) if state_file.exists() else {}
    record("升级成功完成（无故障注入）",
           state.get("state") == "CONFIRMED",
           f"state={state.get('state')}")

    record("confirmed 已更新为新版",
           state.get("confirmed_version") == NEW_VERSION,
           f"confirmed={state.get('confirmed_version')}")

    cur = os.readlink(install / "current")
    record("current 指向新版",
           cur == f"releases/v{NEW_VERSION}", f"current={cur}")

    # 停止旧版后必须清理 pidfile，不能留下指向已死进程的记录
    record("pidfile 已清理",
           read_pidfile(install) is None,
           f"pidfile={read_pidfile(install)}")

    # 清理
    cleanup(proc)

    record("结束后无遗留 Gateway 进程",
           len(gateway_procs()) == 0,
           f"found={gateway_procs()}")

    return True


def scenario_unknown_state():
    """场景二：pidfile 损坏 -> 必须中止，不得切换版本。"""
    print("\n" + "=" * 72)
    print("场景二：pidfile 损坏 -> 状态 UNKNOWN -> 必须中止")
    print("=" * 72)

    install = setup_install()
    if install is None:
        return False

    run_dir = install / "run"
    run_dir.mkdir(parents=True, exist_ok=True)
    (run_dir / "gateway.pid").write_text("NOT-A-VALID-PIDFILE\n")
    (run_dir / "gateway.pid").chmod(0o600)

    before_current = os.readlink(install / "current")
    before_state = json.loads(
        (install / "state" / "ota_state.json").read_text())

    conv = subprocess.run(
        [str(AGENT), "upgrade-test",
         str(ROOT / f"build/ota-test/real-x86/v{NEW_VERSION}"),
         str(install)],
        capture_output=True, text=True, timeout=120,
    )

    out = conv.stdout + conv.stderr
    print(out, flush=True)

    record("识别为 UNKNOWN",
           "Pre-upgrade gateway state: UNKNOWN" in out)
    record("升级被中止", conv.returncode != 0,
           f"exit={conv.returncode}")

    after_current = os.readlink(install / "current")
    record("current 未被修改",
           before_current == after_current,
           f"{before_current} -> {after_current}")

    after_state = json.loads(
        (install / "state" / "ota_state.json").read_text())
    record("状态文件未被修改",
           before_state["state"] == after_state["state"],
           f"state={after_state['state']}")

    # 不得留下任何 Gateway 进程
    record("无 Gateway 进程被启动",
           len(gateway_procs()) == 0,
           f"found={gateway_procs()}")

    cleanup()
    return True


def scenario_stop_fails():
    """场景三：Gateway 无法被停止 -> 必须中止，绝不能切换版本.

    这是 S3 最重要的安全约束。若此时继续切换 current，
    会变成"旧版还在跑，但磁盘上的文件已被换掉"，
    这个状态无法自动收敛。
    """
    print("\n" + "=" * 72)
    print("场景三：Gateway 无法停止 -> 必须中止，不得切换版本")
    print("=" * 72)

    install = setup_install()
    if install is None:
        return False

    if port_listening(GW_PORT):
        record("前置：7000 空闲", False, "端口已被占用")
        return False

    # 启动一个进程并让它忽略 SIGTERM，模拟"停不下来"
    script = "trap '' TERM; while :; do sleep 1; done"
    proc = subprocess.Popen(
        ["/bin/sh", "-c", script],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )

    time.sleep(0.5)

    written = write_pidfile(install, proc.pid)
    record("已为顽固进程写入 pidfile", written)

    if not written:
        cleanup(proc)
        return False

    before_current = os.readlink(install / "current")
    before_state = json.loads(
        (install / "state" / "ota_state.json").read_text())

    conv = subprocess.run(
        [str(AGENT), "upgrade-test",
         str(ROOT / f"build/ota-test/real-x86/v{NEW_VERSION}"),
         str(install)],
        capture_output=True, text=True, timeout=180,
    )

    out = conv.stdout + conv.stderr
    print(out, flush=True)

    record("检测到运行中的实例",
           "Pre-upgrade gateway state: RUNNING" in out)
    record("升级被中止", conv.returncode != 0,
           f"exit={conv.returncode}")

    # 核心断言：绝不能切换版本
    after_current = os.readlink(install / "current")
    record("current 未被切换（关键安全约束）",
           before_current == after_current,
           f"{before_current} -> {after_current}")

    after_state = json.loads(
        (install / "state" / "ota_state.json").read_text())
    record("状态文件未被推进",
           before_state["state"] == after_state["state"],
           f"state={after_state['state']}")

    # 顽固进程应仍在（我们没杀它，只是中止升级）
    record("顽固进程未被硬杀（无 SIGKILL）",
           proc.poll() is None, f"pid={proc.pid}")

    cleanup(proc)
    return True


def main():
    print("=" * 72)
    print("S3 前置检查验收：Gateway 正在运行时执行升级")
    print("=" * 72)

    if not AGENT.exists():
        print(f"缺少 {AGENT}，请先构建 ota_agent_x86", file=sys.stderr)
        return 2

    if not GW.exists():
        print(f"缺少 {GW}，请先 make gateway", file=sys.stderr)
        return 2

    try:
        scenario_running_gateway()
        scenario_unknown_state()
        scenario_stop_fails()
    finally:
        cleanup()

    total = len(results)
    passed = sum(1 for _, ok, _ in results if ok)

    print("\n" + "=" * 72)
    print(f"合计 {total} 项, 通过 {passed}, 失败 {total - passed}")

    if passed != total:
        print("结论: FAIL")
        return 1

    print("结论: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
