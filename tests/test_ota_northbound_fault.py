#!/usr/bin/env python3
"""D23-6-8 北向 TCP 故障与 OTA 自动回滚验收协调器.

职责划分:
  本脚本是"测试协调器", 只负责制造故障与做外部观测;
  升级事务、健康检查、回滚全部由 ota_agent_x86 自己完成。

故障注入方式:
  先 bind 北向上游端口但**不 listen**。
  于是 Gateway 的 connect() 会得到内核返回的 ECONNREFUSED,
  而不是靠环境变量让测试程序提前短路返回失败。

用法:
  python3 tests/test_ota_northbound_fault.py --install DIR
"""

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

HOST = "127.0.0.1"
GW_PORT = 7000
LOG_DIR = Path("build/ota-test")
AGENT = "./bsp/apps/ota_agent/ota_agent_x86"

OLD_VERSION = "0.1.0-test1"
NEW_VERSION = "0.1.1-test1"

results = []


def record(name, ok, detail):
    results.append((name, bool(ok), detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)


def port_listening(port):
    """返回监听该端口而非拒绝连接; 用真实 connect 结果判定。"""
    try:
        with socket.create_connection((HOST, port), timeout=0.3):
            return True
    except ConnectionRefusedError:
        return False
    except OSError:
        return True


def occupied_ports_on_gw():
    """外部观察 7000 是否被占用(不看进程名, 只看内核行为)。"""
    return port_listening(GW_PORT)


def run(cmd, env=None, timeout=120):
    full_env = os.environ.copy()
    if env:
        full_env.update(env)

    print(f"\n$ {' '.join(cmd)}", flush=True)

    started = time.monotonic()

    # Gateway 的 stdout 是全缓冲, 直接管道会把日志攒到退出才吐;
    # 用 stdbuf 行缓冲, 便于观察时序与失败位置。
    if shutil.which("stdbuf") and os.path.basename(cmd[0]) in (
        "ota_agent_x86",
    ):
        cmd = ["stdbuf", "-oL", "-eL"] + cmd

    proc = subprocess.run(
        cmd,
        env=full_env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=timeout,
    )
    elapsed = time.monotonic() - started

    print(proc.stdout, end="", flush=True)
    print(
        f"[cmd exit={proc.returncode}, elapsed={elapsed:.2f}s]",
        flush=True,
    )
    return proc.returncode, proc.stdout, elapsed


def read_state(install):
    path = Path(install) / "state" / "ota_state.json"
    return json.loads(path.read_text())


def current_target(install):
    return os.readlink(Path(install) / "current")


def sha256_of(path):
    import hashlib

    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def snapshot(install):
    """记录运行前后的版本、状态、软链接与文件摘要。"""
    install = Path(install)
    data = {
        "state": read_state(install),
        "current": current_target(install),
        "releases": {},
    }

    for release in sorted((install / "releases").iterdir()):
        binary = release / "smart_gateway"
        manifest = release / "manifest.json"
        data["releases"][release.name] = {
            "binary_sha256": sha256_of(binary),
            "manifest_sha256": sha256_of(manifest),
            "size": binary.stat().st_size,
        }

    return data


def verify_release_identity(install, version, expect_arch):
    """核对 ELF 架构、manifest 声明与真实摘要三者一致。"""
    release = Path(install) / "releases" / f"v{version}"
    binary = release / "smart_gateway"
    manifest = json.loads((release / "manifest.json").read_text())

    ok = True
    detail = []

    head = binary.read_bytes()[:20]
    is_elf = head[:4] == b"\x7fELF"

    if expect_arch == "x86_64":
        elf_ok = is_elf and head[4] == 2 and int.from_bytes(
            head[18:20], "little"
        ) == 62
    else:
        elf_ok = is_elf and head[4] == 1 and int.from_bytes(
            head[18:20], "little"
        ) == 40

    detail.append(f"elf_{expect_arch}={elf_ok}")
    ok &= elf_ok

    manifest_ok = (
        manifest.get("application") == "smart_gateway"
        and manifest.get("version") == version
        and manifest.get("architecture") == "x86_64-linux-gnu"
        and manifest.get("binary") == "smart_gateway"
        and manifest.get("size") == binary.stat().st_size
    )
    detail.append(f"manifest={manifest_ok}")
    ok &= manifest_ok

    hash_ok = sha256_of(binary) == manifest.get("sha256")
    detail.append(f"sha256={hash_ok}")
    ok &= hash_ok

    return ok, ", ".join(detail)


def count_gateway_procs(package_dir=None):
    """统计仍存活的 Gateway 进程, 用 /proc 精确识别。"""
    found = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue

        try:
            exe = os.readlink(entry / "exe")
        except OSError:
            continue

        if not exe.endswith("/smart_gateway") and not exe.endswith(
            "/smart_gateway (deleted)"
        ):
            continue

        found.append((int(entry.name), exe))

    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--install",
        default="build/ota-test/install-x86-fault.2e0Csr",
    )
    parser.add_argument(
        "--package",
        default="build/ota-test/real-x86/v0.1.1-test1",
    )
    parser.add_argument(
        "--prepare",
        action="store_true",
        help="先把安装目录恢复到 CONFIRMED/旧版 current 的初始态",
    )
    args = parser.parse_args()

    install = Path(args.install)
    package = Path(args.package)

    LOG_DIR.mkdir(parents=True, exist_ok=True)

    print("=" * 72)
    print("D23-6-8 北向 TCP 故障与 OTA 自动回滚验收")
    print("=" * 72)

    if args.prepare:
        print("\n--- 准备: 恢复初始状态 ---\n")
        init = LOG_DIR / "ota_baseline_init"
        code, _, _ = run(
            [str(init), str(install / "state")], timeout=30
        )
        if code != 0:
            print("FAIL: 无法恢复初始状态", flush=True)
            return 2

        target = Path(install) / "current"
        if target.is_symlink():
            target.unlink()
        target.symlink_to(f"releases/v{OLD_VERSION}")
        print(f"current -> releases/v{OLD_VERSION}", flush=True)

    # ---------------- 步骤 1: 环境检查 ----------------
    print("\n--- 步骤 1: 环境检查 ---\n")

    before = snapshot(install)
    print(json.dumps(before, indent=2, ensure_ascii=False))

    state0 = before["state"]
    record(
        "初始状态 CONFIRMED",
        state0["state"] == "CONFIRMED",
        f"state={state0['state']}",
    )
    record(
        "初始 current 指向旧版",
        before["current"] == f"releases/v{OLD_VERSION}",
        f"current={before['current']}",
    )
    record(
        "初始 confirmed_version",
        state0["confirmed_version"] == OLD_VERSION,
        f"confirmed={state0['confirmed_version']}",
    )

    for version in (OLD_VERSION, NEW_VERSION):
        ok, detail = verify_release_identity(install, version, "x86_64")
        record(f"{version} ELF/Manifest/SHA-256", ok, detail)

    record(
        "候选包与已安装候选一致",
        sha256_of(package / "smart_gateway")
        == before["releases"][f"v{NEW_VERSION}"]["binary_sha256"],
        "package == releases/v0.1.1-test1",
    )

    record(
        "TCP 7000 空闲",
        not occupied_ports_on_gw(),
        "7000 未被其他进程占用",
    )

    agent_procs = count_gateway_procs()
    record(
        "测试前无遗留 Gateway 进程",
        len(agent_procs) == 0,
        f"found={agent_procs}",
    )

    if any(not ok for _, ok, _ in results):
        print("\n环境检查未通过, 终止测试。", flush=True)
        return 2

    # ---------------- 步骤 2: 制造真实北向故障 ----------------
    print("\n--- 步骤 2: 真实北向故障注入 ---\n")

    # bind 但不 listen: 内核对该端口返回 ECONNREFUSED
    holder = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    holder.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    holder.bind((HOST, 0))
    fault_port = holder.getsockname()[1]

    acceptconn = holder.getsockopt(
        socket.SOL_SOCKET, socket.SO_ACCEPTCONN
    )
    record(
        "北向端口已 bind 但未 listen",
        acceptconn == 0,
        f"port={fault_port}, SO_ACCEPTCONN={acceptconn}",
    )

    try:
        with socket.create_connection((HOST, fault_port), timeout=0.3):
            injection_refused = False
    except ConnectionRefusedError:
        injection_refused = True

    record(
        "该端口真实返回 ECONNREFUSED",
        injection_refused,
        "内核级拒绝, 非测试开关短路",
    )

    if not injection_refused:
        holder.close()
        return 2

    # ---------------- 步骤 3~7: 执行升级事务 ----------------
    print("\n--- 步骤 3~7: 升级事务 (含 L1/L2 与自动回滚) ---\n")

    log_path = LOG_DIR / "d23-6-8-northbound-fault.log"

    try:
        code, output, elapsed = run(
            [
                AGENT,
                "upgrade-test",
                str(package),
                str(install),
            ],
            env={"OTA_UPSTREAM_PORT": str(fault_port)},
            timeout=180,
        )
    finally:
        # 协调器立即释放端口, 让回滚后的旧版能重新连接真实 Server
        holder.close()

    log_path.write_text(output)
    print(f"\n完整日志: {log_path}", flush=True)

    after_txn_state = read_state(install)
    after_txn_current = current_target(install)

    # 记录日志中出现的证据
    def has(line):
        return line in output

    record(
        "升级事务退出码非零",
        code != 0,
        f"exit={code}",
    )
    record(
        "新版 Gateway 已启动",
        "Gateway started: pid=" in output,
        "有 pid 证据",
    )
    record(
        "新版 L1 检查 PASS",
        has("PASS: L1 TCP ready"),
        "7000 就绪",
    )
    record(
        "新版北向真实失败",
        has("FAIL: northbound refused (ECONNREFUSED)"),
        f"port={fault_port}",
    )
    record(
        "南向 DATA 被接受",
        has("PASS: southbound DATA accepted"),
        "解析+入队发生在 Gateway 内部",
    )
    record(
        "新版 L2 检查 FAIL",
        has("FAIL: L2 business health"),
        "北向无业务确认",
    )
    record(
        "新版进程停止并回收",
        "Gateway stop result=0" in output
        and has("PASS: L1 TCP ready"),
        "SIGTERM 后正常回收",
    )
    record(
        "文件系统回滚 PASS",
        has("PASS: rollback completed"),
        "current 恢复到旧版",
    )
    record(
        "旧版重新启动",
        output.count("Gateway started: pid=") >= 2,
        f"启动次数={output.count('Gateway started: pid=')}",
    )
    record(
        "旧版业务恢复",
        has("PASS: old Gateway business recovered"),
        "L1+L2 全部通过",
    )

    # ---------------- 状态与软链接核验 ----------------
    record(
        "OTA 最终状态 ROLLED_BACK",
        after_txn_state["state"] == "ROLLED_BACK",
        f"state={after_txn_state['state']}",
    )
    record(
        "确认版本仍为旧版",
        after_txn_state["confirmed_version"] == OLD_VERSION,
        f"confirmed={after_txn_state['confirmed_version']}",
    )
    record(
        "current 指向旧版",
        after_txn_current == f"releases/v{OLD_VERSION}",
        f"current={after_txn_current}",
    )

    # ---------------- 步骤 8: 资源清理 ----------------
    print("\n--- 步骤 8: 资源清理与状态核验 ---\n")

    time.sleep(0.5)

    leaked = count_gateway_procs()
    record(
        "无遗留 Gateway 进程",
        len(leaked) == 0,
        f"found={leaked}",
    )
    record(
        "TCP 7000 已释放",
        not occupied_ports_on_gw(),
        "端口可重新绑定",
    )

    status_code, status_out, _ = run(
        [AGENT, "status", str(install)], timeout=30
    )
    record(
        "status 命令自洽",
        status_code == 0
        and f"Confirmed : {OLD_VERSION}" in status_out
        and f"Current   : releases/v{OLD_VERSION}" in status_out,
        f"exit={status_code}",
    )

    # 正常升级基线不受影响
    baseline = Path("build/ota-test/install-x86.9L0EFT")

    if baseline.exists():
        bstate = read_state(baseline)
        record(
            "正常升级基线未被修改",
            bstate["state"] == "CONFIRMED"
            and bstate["confirmed_version"] == NEW_VERSION,
            f"state={bstate['state']}, "
            f"confirmed={bstate['confirmed_version']}",
        )

    # 运行前后对比
    after = snapshot(install)

    record(
        "两个发行版文件未被破坏",
        after["releases"] == before["releases"],
        "ELF 与 manifest 摘要一致",
    )

    # ---------------- 汇总 ----------------
    print("\n" + "=" * 72)
    print("验收汇总")
    print("=" * 72)

    failed = [r for r in results if not r[1]]

    for name, ok, detail in results:
        print(f"{'PASS' if ok else 'FAIL':>4}  {name:<34} {detail}")

    print(
        f"\n合计 {len(results)} 项, 通过 {len(results) - len(failed)}, "
        f"失败 {len(failed)}"
    )

    print(f"故障注入端口 (真实 ECONNREFUSED): {fault_port}")
    print(f"升级事务退出码: {code}")
    print(f"事务耗时: {elapsed:.2f}s")

    if failed:
        print("\n结论: FAIL")
        return 1

    print("\n结论: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
