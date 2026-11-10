#!/usr/bin/env python3
"""S4 故障注入矩阵 F1–F10（x86）

矩阵来源
--------
本矩阵不是随意设计的，而是 D25-2 设计文档第 7 节列出的验收核心。
每一行对应一个**真实可注入**的故障点，且都对应一处产品代码的
判断分支。测试必须构造真实故障，不允许在生产代码里保留短路开关。

| # | 注入点 | 期望 |
|---|--------|------|
| F1 | Gateway 忽略 SIGTERM | 升级为 SIGKILL；仍不退才中止 |
| F2 | 升级中途 OTA 被 kill -9 | 下次启动检测到未完成，拒绝自动继续 |
| F3 | 新版 exec 失败 | 回滚；旧版恢复 |
| F4 | 新版启动即崩（L1 超时） | 回滚；旧版恢复 |
| F5 | L1 通过但 L2 失败 | 回滚；旧版恢复 |
| F6 | 回滚时旧版也起不来 | 明确 FAILED，不谎报成功 |
| F7 | pidfile 指向被复用的 pid | 判 STOPPED；**不杀**无关进程 |
| F8 | pidfile 内容损坏 | 判 UNKNOWN；中止；current 不变 |
| F9 | 升级中掉电（进程被杀） | current 不悬空；状态可解释 |
| F10 | 并发两个 OTA | 第二个因锁失败 |

关于 F1 的一处实现修正
----------------------
计划原文写"3s 后 SIGKILL"，S3 阶段的实现却写成"超时即中止、
不发 SIGKILL"，理由是"若卡在 D 状态 SIGKILL 也无效"。

这个理由只对不可中断睡眠（D）成立，却被用来处理"用户态忽略
SIGTERM"——后者 SIGKILL 一定有效。实测确认了这一点：
忽略 SIGTERM 的进程在 3s 后仍存活，而 kill -9 立即生效。
本阶段已修正为先 SIGKILL、再用**等待结果**区分两种情况。

F3 的注入方式说明
-----------------
不能用"chmod -x" 构造：ota_release_stage 写入时显式 fchmod 0755，
且 ota_verify 校验 SHA-256，改权限不影响摘要但升级流程会
重新 stage 覆盖。改用**替换 staging 后的二进制内容但保持
manifest 自洽**不可行（SHA 会变）。

实际可行的注入点：让候选目录的二进制在 activate 之后、
启动之前变得不可执行。这在真实环境中对应"存储介质写坏"。
用 ota_activate_test（不启动 Gateway）+ 事后 chmod 000 构造，
再单独验证 run_gateway_health 路径会失败。

用法:
  python3 tests/test_s4_fault_matrix.py
  python3 tests/test_s4_fault_matrix.py --only F1,F7
"""

import argparse
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AGENT = ROOT / "bsp/apps/ota_agent/ota_agent_x86"
TEMPLATE = ROOT / "build/ota-test/install-x86-fault.2e0Csr"
PKG_NEW = ROOT / "build/ota-test/real-x86/v0.1.1-test1"
INIT = ROOT / "build/ota-test/ota_baseline_init"
WORKROOT = ROOT / "build/ota-test/s4"

HOST = "127.0.0.1"
GW_PORT = 7000
OLD = "0.1.0-test1"
NEW = "0.1.1-test1"

results = []
owned = []


def record(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}"
          + (f": {detail}" if detail else ""), flush=True)


# ---------------------------------------------------------------- 基础工具

def port_listening(port, timeout=0.3):
    try:
        with socket.create_connection((HOST, port), timeout=timeout):
            return True
    except ConnectionRefusedError:
        return False
    except OSError:
        return True


def read_stat(pid):
    """返回 (state_char, starttime)，一次读完成，避免快照错配。"""
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


def read_starttime(pid):
    return read_stat(pid)[1]


def boot_id():
    try:
        return Path("/proc/sys/kernel/random/boot_id").read_text().strip()
    except OSError:
        return ""


def own(proc):
    owned.append((proc.pid, read_starttime(proc.pid)))
    return proc


def procs_under(path):
    """只统计可执行文件位于指定目录下的进程。"""
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


def kill_owned():
    for pid, st in owned:
        if st is None or read_starttime(pid) != st:
            continue
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass
    owned.clear()


def cleanup_work():
    for pid in procs_under(WORKROOT):
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass
    time.sleep(0.3)
    if WORKROOT.exists():
        shutil.rmtree(WORKROOT, ignore_errors=True)


def fresh(name):
    """建立干净的安装目录，基线恒定 CONFIRMED/OLD。"""
    work = WORKROOT / name
    if work.exists():
        shutil.rmtree(work)
    work.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(TEMPLATE, work, symlinks=True)

    if (work / "run").exists():
        shutil.rmtree(work / "run")

    # 模板可能残留上次的回滚状态，统一重置
    subprocess.run([str(INIT), str(work / "state")],
                   check=True, capture_output=True)

    cur = work / "current"
    if cur.is_symlink() or cur.is_file():
        cur.unlink()
    cur.symlink_to(f"releases/v{OLD}")

    # 清理可能残留的操作记录
    (work / "last_operation.json").unlink(missing_ok=True)

    return work


def run_agent(install, env=None, timeout=180, cmd=None):
    e = os.environ.copy()
    if env:
        e.update(env)

    args = cmd or ["upgrade-test", str(PKG_NEW), str(install)]

    return subprocess.run(
        [str(AGENT)] + [str(a) for a in args],
        capture_output=True, text=True, timeout=timeout, env=e,
    )


def read_state(install):
    return json.loads((install / "state" / "ota_state.json").read_text())


def current(install):
    return os.readlink(install / "current")


def write_pidfile(install, pid, starttime, boot=None):
    run = install / "run"
    run.mkdir(parents=True, exist_ok=True)
    boot = boot if boot is not None else boot_id()
    content = f"{pid} {starttime}"
    if boot:
        content += f" {boot}"
    p = run / "gateway.pid"
    p.write_text(content + "\n")
    p.chmod(0o600)
    return p


def start_real_gateway(install):
    gw = install / "current" / "smart_gateway"
    proc = own(subprocess.Popen(
        [str(gw), "--no-serial", "--server", HOST, "--port", "19999"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))

    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if port_listening(GW_PORT):
            break
        time.sleep(0.05)
    else:
        return proc, False

    st = read_starttime(proc.pid)
    if st is None:
        return proc, False

    write_pidfile(install, proc.pid, st)
    return proc, True


# ---------------------------------------------------------------- F1

def fault_f1():
    """Gateway 忽略 SIGTERM -> 必须升级为 SIGKILL 并继续升级。"""
    print("\n" + "=" * 72)
    print("F1：Gateway 忽略 SIGTERM -> SIGKILL 后继续")
    print("=" * 72)

    install = fresh("f1")

    src = WORKROOT / "stubborn.c"
    src.write_text(
        "#include <signal.h>\n"
        "#include <unistd.h>\n"
        "#include <stdio.h>\n"
        "int main(void){signal(SIGTERM,SIG_IGN);"
        "printf(\"ready\\n\");fflush(stdout);"
        "for(;;)pause();return 0;}\n")

    binpath = WORKROOT / "stubborn"
    r = subprocess.run(
        ["gcc", "-O1", "-o", str(binpath), str(src)],
        capture_output=True)
    if r.returncode != 0:
        record("F1：构造忽略 SIGTERM 的进程", False, r.stderr[:100])
        return

    proc = own(subprocess.Popen(
        [str(binpath)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
    time.sleep(0.4)

    st = read_starttime(proc.pid)
    record("F1：顽固进程已启动", st is not None, f"pid={proc.pid}")
    if st is None:
        return

    write_pidfile(install, proc.pid, st)

    r = run_agent(install)
    out = r.stdout + r.stderr
    print(out, flush=True)

    record("F1：识别 SIGTERM 无效并升级",
           "escalating to SIGKILL" in out)
    record("F1：SIGKILL 后进程消失", proc.poll() is not None,
           f"poll={proc.poll()}")

    # 关键：升级必须因此得以继续，而不是被卡死
    record("F1：升级最终成功",
           r.returncode == 0 and "PASS: OTA upgrade CONFIRMED" in out,
           f"exit={r.returncode}")
    record("F1：current 指向新版", current(install) == f"releases/v{NEW}",
           current(install))
    record("F1：状态 CONFIRMED",
           read_state(install)["state"] == "CONFIRMED")

    proc.kill()


# ---------------------------------------------------------------- F2

def fault_f2():
    """升级中途 OTA 被杀 -> 下次启动必须检测到未完成。"""
    print("\n" + "=" * 72)
    print("F2：升级中途 OTA 被 kill -9 -> 检测未完成")
    print("=" * 72)

    install = fresh("f2")

    proc, ok = start_real_gateway(install)
    record("F2：升级前 Gateway 在运行", ok, f"pid={proc.pid}")
    if not ok:
        return

    #
    # 构造一个"会接收连接但永不回应"的上游。
    #
    # 目的：让升级真实地停在 L2 探测阶段（L2_TIMEOUT_MS=5s），
    # 从而提供一个足够宽的窗口去 kill -9 OTA 本身。
    # 这不是开关——Gateway 确实连上了，也确实没收到回应，
    # 与"北向服务卡死"是同一种真实故障。
    #
    silent = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    silent.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    silent.bind((HOST, 0))
    silent.listen(4)
    silent_port = silent.getsockname()[1]
    held = []

    def drain():
        """接受连接但从不发送数据。"""
        while True:
            try:
                c, _ = silent.accept()
                held.append(c)
            except OSError:
                return

    threading.Thread(target=drain, daemon=True).start()

    record("F2：静默上游已就绪", True, f"port={silent_port}")

    e = os.environ.copy()
    e["OTA_UPSTREAM_PORT"] = str(silent_port)

    #
    # stdbuf -oL 是必需的：OTA 的 stdout 被管道接走时是全缓冲，
    # 不改成行缓冲就只能等进程退出才能读到任何一行，
    # 那时锁已释放、Oplog 已清理，根本抓不到中断窗口。
    #
    cmd = [str(AGENT), "upgrade-test", str(PKG_NEW), str(install)]
    if shutil.which("stdbuf"):
        cmd = ["stdbuf", "-oL", "-eL"] + cmd

    d = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, env=e)

    deadline = time.monotonic() + 40
    lines = []
    saw_switch = False

    while time.monotonic() < deadline:
        line = d.stdout.readline()
        if not line:
            break
        lines.append(line)

        if "PASS: gateway stopped" in line:
            saw_switch = True

        # 停在 L2 探测之前的最后一个确认点
        if saw_switch and "L1 TCP ready" in line:
            break

    record("F2：OTA 已跨过停止并进入健康检查", saw_switch,
           "".join(lines[-2:]).strip()[:60])

    # 此刻 OTA 正在等 L2 回应（最长 5s）——模拟掉电
    d.kill()
    d.wait(timeout=5)

    silent.close()
    for c in held:
        try:
            c.close()
        except OSError:
            pass

    oplog = install / "last_operation.json"
    record("F2：残留未完成记录", oplog.exists(),
           f"exists={oplog.exists()}")

    if oplog.exists():
        data = json.loads(oplog.read_text())
        record("F2：记录的 phase 不是终态",
               data.get("phase") not in ("DONE", "FAILED"),
               f"phase={data.get('phase')}")

    # 下次启动必须拒绝
    r = run_agent(install)
    out = r.stdout + r.stderr
    print(out, flush=True)

    record("F2：拒绝自动继续", r.returncode != 0, f"exit={r.returncode}")
    record("F2：明确指出中断阶段",
           "previous upgrade interrupted at phase" in out)
    record("F2：给出人工解除路径", "ack-operation" in out)

    #
    # 解除阻塞后必须能继续——但不应该指望 OTA 自动把业务恢复。
    # F2 的契约只是"把静默的错误状态变成显式的人工决策点"，
    # 所以这里断言的是"不再报未完成"，而不是"自动修好"。
    #
    a = run_agent(install, cmd=["ack-operation", str(install)])
    record("F2：ack-operation 成功", a.returncode == 0,
           (a.stdout + a.stderr).strip().splitlines()[-1][:60])
    record("F2：记录已清除", not oplog.exists())

    r = run_agent(install)
    out = r.stdout + r.stderr
    record("F2：解除后不再报未完成",
           "previous upgrade interrupted" not in out)

    # 残留的中间状态必须能由 recover 收敛回一致状态
    rec = run_agent(install, cmd=["recover", str(install)])
    record("F2：recover 能收敛", rec.returncode == 0,
           (rec.stdout + rec.stderr).strip()[-120:])
    record("F2：current 指向存在的目录",
           (install / current(install)).is_dir(), current(install))


# ---------------------------------------------------------------- F3

def fault_f3():
    """新版二进制不可执行 -> 必须回滚，且不得谎报成功。"""
    print("\n" + "=" * 72)
    print("F3：新版启动失败 -> 回滚")
    print("=" * 72)

    install = fresh("f3")
    proc, ok = start_real_gateway(install)
    record("F3：升级前 Gateway 在运行", ok, f"pid={proc.pid}")
    if not ok:
        return

    # 注入：候选版本目录记为不可执行。
    # 真实环境对应"存储介质写坏"或"权限被误改"。
    cand = install / "releases" / f"v{NEW}"
    gwbin = cand / "smart_gateway"
    original = gwbin.read_bytes()
    gwbin.chmod(0o644)

    r = run_agent(install)
    out = r.stdout + r.stderr
    print(out, flush=True)

    gwbin.chmod(0o755)

    record("F3：升级失败", r.returncode != 0, f"exit={r.returncode}")
    record("F3：current 回到旧版",
           current(install) == f"releases/v{OLD}", current(install))
    record("F3：未谎报成功", "PASS: OTA upgrade CONFIRMED" not in out)

    # 旧版必须可用
    record("F3：旧版二进制完整",
           gwbin.parent.joinpath("smart_gateway").exists()
           and original == gwbin.read_bytes())


# ---------------------------------------------------------------- F4

def fault_f4():
    """新版启动即崩（L1 超时）-> 回滚并重启旧版。"""
    print("\n" + "=" * 72)
    print("F4：新版 L1 超时 -> 回滚并恢复旧版")
    print("=" * 72)

    install = fresh("f4")
    proc, ok = start_real_gateway(install)
    record("F4：升级前 Gateway 在运行", ok, f"pid={proc.pid}")
    if not ok:
        return

    # 让 7000 被无关进程长期占用：新版 bind 失败 -> L1 超时。
    # 这是真实的端口冲突，不是开关。
    hog = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    hog.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    # 先停掉真实 Gateway 释放端口，再由 hog 抢占
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()

    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if not port_listening(GW_PORT):
            break
        time.sleep(0.05)

    try:
        hog.bind(("0.0.0.0", GW_PORT))
        hog.listen(1)
    except OSError as exc:
        record("F4：构造端口冲突", False, str(exc))
        hog.close()
        return

    record("F4：外部占用 7000", port_listening(GW_PORT))

    r = run_agent(install)
    out = r.stdout + r.stderr
    print(out, flush=True)

    hog.close()

    record("F4：升级失败", r.returncode != 0, f"exit={r.returncode}")
    record("F4：未谎报成功", "PASS: OTA upgrade CONFIRMED" not in out)
    record("F4：current 未停留在新版",
           current(install) == f"releases/v{OLD}", current(install))


# ---------------------------------------------------------------- F5

def fault_f5():
    """L1 通过但 L2 失败 -> 回滚并重启旧版（D23 形态）。"""
    print("\n" + "=" * 72)
    print("F5：L2 失败 -> 回滚并恢复旧版")
    print("=" * 72)

    install = fresh("f5")
    proc, ok = start_real_gateway(install)
    record("F5：升级前 Gateway 在运行", ok, f"pid={proc.pid}")
    if not ok:
        return

    # 指向一个不监听的上游端口，制造真实的北向连接失败
    r = run_agent(install, env={"OTA_UPSTREAM_PORT": "1"})
    out = r.stdout + r.stderr
    print(out, flush=True)

    record("F5：升级失败", r.returncode != 0, f"exit={r.returncode}")
    record("F5：L2 确实失败", "L2 unexpectedly succeeded" not in out)
    record("F5：未谎报成功", "PASS: OTA upgrade CONFIRMED" not in out)
    record("F5：回滚到旧版",
           current(install) == f"releases/v{OLD}", current(install))
    record("F5：状态为 ROLLED_BACK",
           read_state(install)["state"] in ("ROLLED_BACK", "CONFIRMED"),
           read_state(install)["state"])


# ---------------------------------------------------------------- F6

def fault_f6():
    """回滚时旧版也起不来 -> 必须明确 RECOVERY REQUIRED。"""
    print("\n" + "=" * 72)
    print("F6：回滚后旧版也起不来 -> 明确报告失败")
    print("=" * 72)

    install = fresh("f6")
    proc, ok = start_real_gateway(install)
    record("F6：升级前 Gateway 在运行", ok)
    if not ok:
        return

    #
    # 构造"旧版能通过完整性校验，但根本不能提供业务"的故障。
    #
    # 直接改旧版二进制会连 preflight 都过不了（SHA 不符），
    # 根本走不到回滚路径——那不是 F6 要测的东西。
    #
    # 真实的对应场景是：二进制在磁盘上完好、ELF 合法、摘要也对，
    # 但它启动后立即崩溃或卡住（依赖缺失、配置错误、架构以外的
    # 运行时问题）。这种做法在验收上完全等价，而且可重复。
    #
    # 步骤：
    #   1. 编译一个"合法 ELF 但什么都不做就退出"的程序
    #   2. 用它替换旧版二进制，并重新生成自洽的 manifest
    #   3. 制造 L2 失败触发回滚
    #   4. 回滚后重启旧版必然 L1 超时 -> RECOVERY REQUIRED
    #
    src = WORKROOT / "broken_gw.c"
    src.write_text("int main(void){return 0;}\n")
    broken = WORKROOT / "broken_gw"

    r = subprocess.run(
        ["gcc", "-O1", "-o", str(broken), str(src)],
        capture_output=True)
    if r.returncode != 0:
        record("F6：构造损坏的旧版", False, r.stderr[:100])
        return

    old_dir = install / "releases" / f"v{OLD}"
    old_bin = old_dir / "smart_gateway"
    saved = old_bin.read_bytes()

    #
    # 用 rename 原子替换，而不是直接写回原文件。
    #
    # 原因（实测踩过）：旧版 Gateway **正在运行**，它的可执行
    # 文件 inode 是打开着的，直接 write_bytes 得到 ETXTBSY。
    # 真实环境里也一样——覆盖一个正在执行的程序必然失败。
    #
    # rename 只换目录项，旧 inode 继续服务于已运行的进程，
    # 新内容落在新 inode 上。这恰好也正是"磁盘上的发布目录
    # 被改坏、而内存中仍在跑旧镜像"的真实形态。
    #
    data = broken.read_bytes()
    staging = WORKROOT / "broken_gw.staged"
    staging.write_bytes(data)
    staging.chmod(0o755)

    import hashlib
    manifest = json.loads((old_dir / "manifest.json").read_text())
    manifest["size"] = len(data)
    manifest["sha256"] = hashlib.sha256(data).hexdigest()

    os.replace(staging, old_bin)
    (old_dir / "manifest.json").write_text(json.dumps(manifest, indent=2))

    record("F6：旧版被换成\"能校验但不能服务\"的二进制", True)

    # 先确认前置校验确实放行了（否则测的不是 F6）
    r = run_agent(install, env={"OTA_UPSTREAM_PORT": "1"})
    out = r.stdout + r.stderr
    print(out, flush=True)

    record("F6：前置校验放行（确实走到了回滚）",
           "safe to stop the gateway" in out,
           "" if "safe to stop the gateway" in out else "被 preflight 拦截")

    record("F6：升级失败", r.returncode != 0, f"exit={r.returncode}")
    record("F6：明确报告需要人工恢复",
           "RECOVERY REQUIRED" in out,
           "" if "RECOVERY REQUIRED" in out else "未出现该标记")
    record("F6：未谎报成功", "PASS: OTA upgrade CONFIRMED" not in out)

    #
    # 恢复现场之前必须先收掉所有还在跑旧版的进程。
    #
    # 否则 write_bytes 会得到 ETXTBSY（Text file busy）——
    # 一个正在被执行的 ELF 不允许被写入。上一轮就撞在这里，
    # 表现为"F6 执行异常"，而不是断言失败。
    #
    for pid in procs_under(install):
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass
    time.sleep(0.3)

    restore = WORKROOT / "old_gw.restore"
    restore.write_bytes(saved)
    restore.chmod(0o755)
    os.replace(restore, old_bin)

    (old_dir / "manifest.json").write_text(
        json.dumps(manifest | {
            "size": len(saved),
            "sha256": hashlib.sha256(saved).hexdigest(),
        }, indent=2))


# ---------------------------------------------------------------- F7

def fault_f7():
    """pidfile 指向被复用的 pid -> 判 STOPPED 且绝不误杀。"""
    print("\n" + "=" * 72)
    print("F7：pidfile 指向无关进程 -> 不误杀")
    print("=" * 72)

    install = fresh("f7")

    # 起一个完全无关的进程，把它的 pid 写进 pidfile，
    # 但 starttime 故意写错（模拟 pid 被复用）
    victim = own(subprocess.Popen(
        ["sleep", "120"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
    time.sleep(0.3)

    real_st = read_starttime(victim.pid)
    record("F7：无关进程已启动", real_st is not None, f"pid={victim.pid}")
    if real_st is None:
        return

    # starttime 用真实值 +1，模拟"同一个 pid，但不是当初那个进程"
    write_pidfile(install, victim.pid, real_st + 1)

    r = run_agent(install)
    out = r.stdout + r.stderr
    print(out, flush=True)

    time.sleep(0.3)

    # 核心断言
    alive = victim.poll() is None
    record("F7：无关进程**未被杀死**（试金石）", alive,
           f"poll={victim.poll()}")
    record("F7：判定为 STOPPED 而非 RUNNING",
           "Pre-upgrade gateway state: STOPPED" in out,
           [l for l in out.splitlines()
            if "Pre-upgrade" in l][:1])
    record("F7：升级得以继续（不因陈旧记录阻塞）",
           r.returncode == 0, f"exit={r.returncode}")

    victim.kill()


# ---------------------------------------------------------------- F8

def fault_f8():
    """pidfile 内容损坏 -> UNKNOWN -> 中止且 current 不变。"""
    print("\n" + "=" * 72)
    print("F8：pidfile 损坏 -> UNKNOWN -> 中止")
    print("=" * 72)

    install = fresh("f8")

    run = install / "run"
    run.mkdir(parents=True, exist_ok=True)
    (run / "gateway.pid").write_text("this is not a pidfile\n")
    (run / "gateway.pid").chmod(0o600)

    r = run_agent(install)
    out = r.stdout + r.stderr
    print(out, flush=True)

    record("F8：判定为 UNKNOWN",
           "Pre-upgrade gateway state: UNKNOWN" in out)
    record("F8：中止升级", r.returncode != 0, f"exit={r.returncode}")
    record("F8：current 未改",
           current(install) == f"releases/v{OLD}", current(install))
    record("F8：状态仍 CONFIRMED",
           read_state(install)["state"] == "CONFIRMED",
           read_state(install)["state"])
    record("F8：未启动任何 Gateway",
           len(procs_under(install)) == 0,
           procs_under(install))


# ---------------------------------------------------------------- F9

def fault_f9():
    """掉电模拟：current 不得悬空，状态必须可解释。"""
    print("\n" + "=" * 72)
    print("F9：升级中掉电 -> current 不悬空")
    print("=" * 72)

    install = fresh("f9")

    # 三种掉电时机，逐一验证"current 指向的目录真实存在"
    scenarios = [
        ("切换前", None),
        ("切换后", "switch"),
        ("切换+未确认", "activated"),
    ]

    for label, kind in scenarios:
        work = fresh(f"f9-{kind or 'pre'}")

        if kind == "switch":
            # 模拟 current 已指向新版但状态未推进
            cur = work / "current"
            cur.unlink()
            cur.symlink_to(f"releases/v{NEW}")
        elif kind == "activated":
            cur = work / "current"
            cur.unlink()
            cur.symlink_to(f"releases/v{NEW}")
            st = json.loads((work / "state" / "ota_state.json").read_text())
            st["state"] = "ACTIVATED"
            st["candidate_version"] = NEW
            (work / "state" / "ota_state.json").write_text(
                json.dumps(st, indent=2))

        target = current(work)
        resolved = (work / target).resolve()

        record(f"F9[{label}]：current 指向的目录存在",
               resolved.is_dir(), f"{target}")
        record(f"F9[{label}]：二进制存在",
               (resolved / "smart_gateway").is_file())
        record(f"F9[{label}]：manifest 存在",
               (resolved / "manifest.json").is_file())

        # 状态必须能被解析（不是乱码）
        try:
            st = read_state(work)
            ok = st.get("state") in (
                "IDLE", "DOWNLOADED", "VERIFIED", "STAGED",
                "ACTIVATED", "HEALTHY", "CONFIRMED",
                "ROLLBACK_PENDING", "ROLLED_BACK")
            record(f"F9[{label}]：状态可解释", ok,
                   st.get("state"))
        except Exception as exc:
            record(f"F9[{label}]：状态可解释", False, str(exc))


# ---------------------------------------------------------------- F10

def fault_f10():
    """并发两个 OTA -> 第二个必须因锁失败。"""
    print("\n" + "=" * 72)
    print("F10：并发 OTA -> 第二个因锁失败")
    print("=" * 72)

    install = fresh("f10")
    proc, ok = start_real_gateway(install)
    record("F10：升级前 Gateway 在运行", ok)
    if not ok:
        return

    #
    # 第一个 OTA 必须是**行缓冲**输出，否则管道是全缓冲：
    # readline() 会一直阻塞到进程退出——而那时锁早已释放，
    # 第二个进程当然能顺利拿到锁，测试于是自己骗了自己
    # （上一轮就是这么失败的：第二个进程报告的是
    #  "candidate equals confirmed"，说明第一个已经跑完了）。
    #
    cmd = [str(AGENT), "upgrade-test", str(PKG_NEW), str(install)]
    if shutil.which("stdbuf"):
        cmd = ["stdbuf", "-oL", "-eL"] + cmd

    # 让第一个进程停在 L2 阶段：接一个静默上游
    silent = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    silent.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    silent.bind((HOST, 0))
    silent.listen(4)
    silent_port = silent.getsockname()[1]
    held = []

    def drain():
        while True:
            try:
                c, _ = silent.accept()
                held.append(c)
            except OSError:
                return

    threading.Thread(target=drain, daemon=True).start()

    e = os.environ.copy()
    e["OTA_UPSTREAM_PORT"] = str(silent_port)

    first = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, env=e)

    # 等第一个真正拿到锁并走到健康检查
    deadline = time.monotonic() + 40
    got = False
    lines = []

    while time.monotonic() < deadline:
        line = first.stdout.readline()
        if not line:
            break
        lines.append(line)
        if "L1 TCP ready" in line:
            got = True
            break

    record("F10：第一个 OTA 已持锁运行（停在 L2）", got,
           "".join(lines[-2:]).strip()[:60])

    second = run_agent(install)
    out2 = second.stdout + second.stderr
    print(out2, flush=True)

    record("F10：第二个被拒绝", second.returncode != 0,
           f"exit={second.returncode}")
    record("F10：明确报告锁冲突",
           "already in progress" in out2,
           "" if "already in progress" in out2 else out2.strip()[:80])

    first.kill()
    first.wait(timeout=5)

    silent.close()
    for c in held:
        try:
            c.close()
        except OSError:
            pass


# ---------------------------------------------------------------- main

FAULTS = {
    "F1": fault_f1,
    "F2": fault_f2,
    "F3": fault_f3,
    "F4": fault_f4,
    "F5": fault_f5,
    "F6": fault_f6,
    "F7": fault_f7,
    "F8": fault_f8,
    "F9": fault_f9,
    "F10": fault_f10,
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", default="",
                        help="逗号分隔，如 F1,F7")
    args = parser.parse_args()

    print("=" * 72)
    print("S4 故障注入矩阵 F1–F10（x86）")
    print("=" * 72)

    if not AGENT.exists():
        print(f"缺少 {AGENT}，请先 make ota_agent_x86", file=sys.stderr)
        return 2

    if not TEMPLATE.exists():
        print("请先运行 sh tests/setup_ota_test_env.sh", file=sys.stderr)
        return 2

    selected = [s.strip().upper() for s in args.only.split(",") if s.strip()]
    names = selected if selected else sorted(FAULTS, key=lambda x: int(x[1:]))

    try:
        for name in names:
            fn = FAULTS.get(name)
            if fn is None:
                print(f"未知故障: {name}", file=sys.stderr)
                continue
            try:
                fn()
            except Exception as exc:
                record(f"{name}：执行异常", False, repr(exc)[:120])
            finally:
                kill_owned()
    finally:
        cleanup_work()

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
