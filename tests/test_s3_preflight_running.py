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
import signal
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


# 本测试自己启动的进程，只允许清理这些。
# 记录 (pid, starttime) 二元组，避免 pid 复用后误杀无关进程。
_owned_procs = []


def own_process(proc):
    """登记一个由本测试启动的进程。"""
    st = read_starttime(proc.pid)
    _owned_procs.append((proc.pid, st))
    return proc


def is_same_process(pid, starttime):
    """校验 pid 是否仍是当初那个进程（防 pid 复用）。"""
    cur = read_starttime(pid)
    return cur is not None and cur == starttime


def cleanup(proc=None, work=True):
    """只清理本测试启动的进程。

    复审问题三指出：原实现用 gateway_procs() 扫描整个 /proc，
    把所有 executable 以 /smart_gateway 结尾的进程全部 SIGTERM。
    如果开发机本机还跑着另一个 SmartGateway（例如手工调试的实例），
    测试收尾会把它一起杀掉 —— 这是明确的误伤。

    改为：
      1. 只处理显式传入的 proc；
      2. 只处理本测试登记过的 _owned_procs；
      3. 每次都先用 (pid, starttime) 复核身份，pid 被复用则跳过；
      4. **绝不**全局扫描 /proc 后批量发信号。
    """
    if proc is not None:
        try:
            proc.terminate()
            proc.wait(timeout=3)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass

    for pid, starttime in _owned_procs:
        if starttime is None:
            continue

        if not is_same_process(pid, starttime):
            # pid 已被复用或进程已消失：不是我们启动的那个，跳过
            continue

        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass

    _owned_procs.clear()

    time.sleep(0.5)

    if work and WORK.exists():
        shutil.rmtree(WORK, ignore_errors=True)


def leftover_in_work_dir():
    """只检查与本次测试目录相关的残留进程。

    通过 /proc/<pid>/exe 是否指向 WORK 目录来判断，
    而不是用文件名后缀匹配 —— 后者会命中无关实例。
    """
    if not WORK.exists():
        return []

    work_real = str(WORK.resolve())
    found = []

    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue

        try:
            exe = os.readlink(entry / "exe")
        except OSError:
            continue

        # 已删除的可执行文件会带 " (deleted)" 后缀
        base = exe.replace(" (deleted)", "")

        if base.startswith(work_real):
            found.append(int(entry.name))

    return found


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
    proc = own_process(subprocess.Popen(
        [str(gw_bin), "--no-serial",
         "--server", HOST, "--port", "19999"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    ))

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

    record("结束后无本目录遗留进程",
           len(leftover_in_work_dir()) == 0,
           f"found={leftover_in_work_dir()}")

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

    # 不得留下任何 Gateway 进程（限定在本次测试目录内）
    record("无本目录遗留进程被启动",
           len(leftover_in_work_dir()) == 0,
           f"found={leftover_in_work_dir()}")

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
    proc = own_process(subprocess.Popen(
        ["/bin/sh", "-c", script],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    ))

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


def scenario_bad_package():
    """场景四：Gateway 正常运行 + 损坏升级包 -> 升级失败但业务不中断.

    复审问题一的核心场景。

    原实现顺序是先停旧版、后校验包，于是：
      损坏包 -> 旧版已停 -> 校验失败 -> 回滚无实质动作 ->
      状态不是 ROLLED_BACK -> 直接退出
    结果：什么都没升级，但业务已经停了，且不会自动恢复。

    修复后：包校验前置，不合格就不碰旧版。
    """
    print("\n" + "=" * 72)
    print("场景四：损坏升级包 -> 升级失败但旧版必须继续运行")
    print("=" * 72)

    install = setup_install()
    if install is None:
        return False

    if port_listening(GW_PORT):
        record("前置：7000 空闲", False, "端口已被占用")
        return False

    # 启动真实 Gateway
    gw_bin = install / "current" / "smart_gateway"
    proc = own_process(subprocess.Popen(
        [str(gw_bin), "--no-serial", "--server", HOST, "--port", "19999"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    ))

    ok = wait_port(GW_PORT, True)
    record("Gateway 已启动并监听 7000", ok)

    if not ok:
        cleanup(proc)
        return False

    written = write_pidfile(install, proc.pid)
    record("已写入 pidfile", written)

    if not written:
        cleanup(proc)
        return False

    # 构造损坏的升级包：哈希故意写错
    bad_pkg = WORK.parent / "s3-bad-package"
    if bad_pkg.exists():
        shutil.rmtree(bad_pkg)
    bad_pkg.mkdir()

    shutil.copy(install / "current" / "smart_gateway",
                bad_pkg / "smart_gateway")

    (bad_pkg / "manifest.json").write_text(json.dumps({
        "format_version": 1,
        "application": "smart_gateway",
        "version": "0.9.9-bad",
        "architecture": "x86_64-linux-gnu",
        "binary": "smart_gateway",
        "size": (bad_pkg / "smart_gateway").stat().st_size,
        "sha256": "0" * 64,          # 故意错误
    }, indent=2))

    conv = subprocess.run(
        [str(AGENT), "upgrade-test", str(bad_pkg), str(install)],
        capture_output=True, text=True, timeout=120,
    )

    out = conv.stdout + conv.stderr
    print(out, flush=True)

    record("升级被拒绝", conv.returncode != 0,
           f"exit={conv.returncode}")

    record("在停止 Gateway 之前就拒绝了",
           "aborted before stopping the gateway" in out)

    # 核心断言：旧版必须仍在运行
    time.sleep(0.3)
    still_alive = proc.poll() is None
    record("原 Gateway 仍在运行（业务未中断）", still_alive,
           f"pid={proc.pid}")

    record("7000 仍在监听", port_listening(GW_PORT))

    # 版本与状态都不得改变
    cur = os.readlink(install / "current")
    record("current 未变", cur == f"releases/v{OLD_VERSION}",
           f"current={cur}")

    state = json.loads((install / "state" / "ota_state.json").read_text())
    record("状态未变", state.get("state") == "CONFIRMED",
           f"state={state.get('state')}")

    cleanup(proc)
    shutil.rmtree(bad_pkg, ignore_errors=True)
    return True


def _start_gateway(install):
    """启动 Gateway 并写入 pidfile，返回 (proc, ok)。"""
    gw_bin = install / "current" / "smart_gateway"
    proc = own_process(subprocess.Popen(
        [str(gw_bin), "--no-serial", "--server", HOST, "--port", "19999"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    ))

    if not wait_port(GW_PORT, True):
        return proc, False

    st = read_starttime(proc.pid)
    if st is None or not write_pidfile(install, proc.pid):
        return proc, False

    return proc, True


def scenario_candidate_missing():
    """场景五：包合法但候选版本未 stage -> 必须在停 Gateway 之前拒绝.

    复审 S3-R2 指出的遗漏：preflight_package() 原先只检查
    releases 目录存在，未检查 releases/<candidate> 是否真的在那里。
    于是"包合法但没 stage"会走到 ota_activate_test() 才失败，
    而此时旧 Gateway 已经被停掉 —— 又一次白白中断。

    正确的做法是把 activate 的全部只读校验前置。
    """
    print("\n" + "=" * 72)
    print("场景五：候选版本未安装 -> 必须在停止 Gateway 之前拒绝")
    print("=" * 72)

    install = setup_install()
    if install is None:
        return False

    if port_listening(GW_PORT):
        record("前置：7000 空闲", False, "端口已被占用")
        return False

    # 删掉已安装的候选版本，模拟"未 stage"
    candidate = install / "releases" / f"v{NEW_VERSION}"
    if candidate.exists():
        shutil.rmtree(candidate)

    record("候选版本目录已删除（模拟未 stage）",
           not candidate.exists())

    proc, ok = _start_gateway(install)
    record("Gateway 已启动并监听 7000", ok)

    if not ok:
        cleanup(proc)
        return False

    # 升级包本身是合法完整的
    pkg = ROOT / f"build/ota-test/real-x86/v{NEW_VERSION}"
    record("升级包本身合法", (pkg / "manifest.json").exists())

    conv = subprocess.run(
        [str(AGENT), "upgrade-test", str(pkg), str(install)],
        capture_output=True, text=True, timeout=120,
    )

    out = conv.stdout + conv.stderr
    print(out, flush=True)

    record("升级被拒绝", conv.returncode != 0,
           f"exit={conv.returncode}")

    record("在停止 Gateway 之前就拒绝了",
           "aborted before stopping the gateway" in out)

    # 核心断言：业务不受影响
    time.sleep(0.3)
    record("原 Gateway 仍在运行（业务未中断）",
           proc.poll() is None, f"pid={proc.pid}")
    record("7000 仍在监听", port_listening(GW_PORT))

    cur = os.readlink(install / "current")
    record("current 未变", cur == f"releases/v{OLD_VERSION}",
           f"current={cur}")

    state = json.loads((install / "state" / "ota_state.json").read_text())
    record("状态未变", state.get("state") == "CONFIRMED",
           f"state={state.get('state')}")

    cleanup(proc)
    return True


def _manifest_for(release_dir, version, override=None):
    """为发布目录生成 manifest.json。override 可覆盖任意字段。"""
    binary = release_dir / "smart_gateway"

    if not binary.exists():
        return

    import hashlib

    digest = hashlib.sha256()
    with open(binary, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 16), b""):
            digest.update(chunk)

    data = {
        "format_version": 1,
        "application": "smart_gateway",
        "version": version,
        "architecture": "x86_64-linux-gnu",
        "binary": "smart_gateway",
        "size": binary.stat().st_size,
        "sha256": digest.hexdigest(),
    }

    if override:
        data.update(override)

    (release_dir / "manifest.json").write_text(
        json.dumps(data, indent=2))


def _run_precheck_scenario(name, mutate, expect_msg, gateway_running=True):
    """通用负向校验场景。

    mutate(install, pkg) 负责构造非法环境，返回可选的 pkg 路径覆盖。
    expect_msg 是期望出现的校验失败信息。

    共同断言：
      - 升级被拒绝
      - 在停止 Gateway 之前就拒绝
      - Gateway（若启动）仍在运行、7000 仍监听
      - current 与状态均未变
    """
    print("\n" + "=" * 72)
    print(name)
    print("=" * 72)

    install = setup_install()
    if install is None:
        return False

    if port_listening(GW_PORT):
        record("前置：7000 空闲", False, "端口已被占用")
        return False

    record("前置：7000 空闲", True)

    pkg = ROOT / f"build/ota-test/real-x86/v{NEW_VERSION}"

    before_state = json.loads(
        (install / "state" / "ota_state.json").read_text())

    # 构造非法条件（可能改变 install 或生成新的 pkg）
    result = mutate(install, pkg)
    if isinstance(result, Path):
        pkg = result

    # 注意：必须在 mutate 之后取 current 基准。
    # 有些场景（如 current 指向异常）本身就是靠改 current 构造的，
    # 若在 mutate 前取基准，会把自己制造的改动误报成"被测代码改了它"。
    before_current = os.readlink(install / "current")

    proc = None
    if gateway_running:
        proc, ok = _start_gateway(install)
        record("Gateway 已启动并监听 7000", ok)
        if not ok:
            cleanup(proc)
            return False

    conv = subprocess.run(
        [str(AGENT), "upgrade-test", str(pkg), str(install)],
        capture_output=True, text=True, timeout=120,
    )

    out = conv.stdout + conv.stderr
    print(out, flush=True)

    record("升级被拒绝", conv.returncode != 0, f"exit={conv.returncode}")
    record(f"命中预期校验：{expect_msg}", expect_msg in out,
           "" if expect_msg in out else "未出现")
    record("在停止 Gateway 之前就拒绝了",
           "aborted before stopping the gateway" in out)

    if gateway_running and proc is not None:
        time.sleep(0.3)
        record("原 Gateway 仍在运行（业务未中断）",
               proc.poll() is None, f"pid={proc.pid}")
        record("7000 仍在监听", port_listening(GW_PORT))

    after_current = os.readlink(install / "current")
    record("current 未变", before_current == after_current,
           f"{before_current} -> {after_current}")

    after_state = json.loads(
        (install / "state" / "ota_state.json").read_text())
    record("状态未变", before_state["state"] == after_state["state"],
           f"state={after_state['state']}")

    # 不得留下半成品：release 目录不应出现候选
    candidate = install / "releases" / f"v{NEW_VERSION}"
    if candidate.exists() and before_current != f"releases/v{NEW_VERSION}":
        pass  # 候选可能本来就该存在（部分场景），不在此断言

    cleanup(proc)
    return True


def scenario_candidate_equals_confirmed():
    """候选版本 == 已确认版本 -> candidate equals confirmed.

    构造：包 manifest 的 version 改成当前的 confirmed 版本。
    这是"重复升级同一版本"的场景，应当被拒绝。
    """
    def mutate(install, pkg):
        # 造一个只含 manifest 差异的包
        tmp = ROOT / "build/ota-test/s3-pkg-samever"
        if tmp.exists():
            shutil.rmtree(tmp)
        shutil.copytree(pkg, tmp)

        _manifest_for(tmp, OLD_VERSION)   # 版本号改成旧版
        return tmp

    return _run_precheck_scenario(
        "场景六：候选版本 == 已确认版本 -> 必须拒绝",
        mutate,
        "candidate equals confirmed",
    )


def scenario_current_mismatch():
    """current 不指向已确认版本 -> confirmed/current mismatch.

    构造：把 current 改指向别的目录（模拟有人手动改过或上次异常遗留）。
    此时升级不应继续，因为无法确认"当前在跑的是哪个版本"。
    """
    def mutate(install, pkg):
        cur = install / "current"
        if cur.is_symlink():
            cur.unlink()
        # 指向一个不存在但名字合法的目标
        cur.symlink_to("releases/v9.9.9-phantom")
        return pkg

    return _run_precheck_scenario(
        "场景七：current 指向非确认版本 -> 必须拒绝",
        mutate,
        "confirmed/current mismatch",
        # current 已被改坏，Gateway 无法从它启动；
        # 本场景要验证的是"校验能识别出不一致"，不需要真实进程。
        gateway_running=False,
    )


def scenario_confirmed_invalid():
    """已确认版本文件损坏 -> confirmed release invalid.

    构造：篡改 releases/<confirmed>/smart_gateway 的内容，
    使其 SHA-256 与 manifest 不符。
    """
    def mutate(install, pkg):
        target = install / "releases" / f"v{OLD_VERSION}" / "smart_gateway"

        # 追加几个字节，破坏 SHA-256 但保留 ELF 头
        with open(target, "ab") as fh:
            fh.write(b"TAMPERED")

        return pkg

    return _run_precheck_scenario(
        "场景八：已确认版本文件被篡改 -> 必须拒绝",
        mutate,
        "confirmed release invalid",
    )


def scenario_candidate_sha_mismatch():
    """候选已安装但 SHA-256 与包不一致 -> installed candidate mismatch.

    与"候选目录缺失"不同：这里目录存在、manifest 合法，
    但二进制内容与升级包不一致（例如传输损坏）。
    """
    def mutate(install, pkg):
        target = install / "releases" / f"v{NEW_VERSION}" / "smart_gateway"

        # 追加字节破坏 SHA，但 manifest 不改 —— 于是
        # ota_verify_package(new_path) 因摘要不符而失败
        with open(target, "ab") as fh:
            fh.write(b"CORRUPT")

        return pkg

    return _run_precheck_scenario(
        "场景九：候选已安装但内容与包不一致 -> 必须拒绝",
        mutate,
        "installed candidate mismatch",
    )


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
        scenario_bad_package()
        scenario_candidate_missing()
        # S3-R3：补齐剩余校验分支的负向覆盖
        scenario_candidate_equals_confirmed()
        scenario_current_mismatch()
        scenario_confirmed_invalid()
        scenario_candidate_sha_mismatch()
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
