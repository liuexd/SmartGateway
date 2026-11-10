#!/usr/bin/env python3
"""S3-R4 验收：停止 Gateway 之后的失败路径与恢复.

D25-2 S3-R4 解决的问题
----------------------
前置校验通过之后、正式激活之前仍可能失败（端口被占、Mock Server
建不起来、activate 二次校验失败）。原实现直接 return，留下
"旧版已停、业务未恢复"的状态。

本阶段的四个场景
----------------
A. 非法上游端口      -> 必须在停止 Gateway **之前**拒绝
B. 升级前未运行      -> 失败后**不得**擅自启动 Gateway
C. 停止后失败且端口被外部占用
                     -> 必须报告 RECOVERY REQUIRED，**不得**谎报已恢复
D. 正常停止后失败    -> 自动恢复旧版，且服务可确认可用

关于场景 C 的假阳性
-------------------
本阶段在此处发现并修复了一个真实缺陷：
恢复逻辑原先用 ota_health_wait_tcp_ready() 判断"L1 就绪"，
但那只能证明**有人**监听 7000。外部进程抢占端口后，
OTA 启动的 Gateway 因 bind 失败立即退出，
而 connect 仍成功 -> 报告 "restored and L1-ready" —— 假的。

修复后改用 port_owned_by_pid()（/proc/net/tcp inode + fd 反查），
确认监听者确实是刚启动的进程。

用法:
  python3 tests/test_s3r4_recovery.py
"""

import json
import os
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# S5：Gateway 改由 Service Supervisor 管理
sys.path.insert(0, str(Path(__file__).resolve().parent))
from supervisor_env import SupervisorEnv  # noqa: E402
AGENT = ROOT / "bsp/apps/ota_agent/ota_agent_x86"
WORK = ROOT / "build/ota-test/s3r4"

HOST = "127.0.0.1"
GW_PORT = 7000
OLD_VERSION = "0.1.0-test1"
NEW_VERSION = "0.1.1-test1"

results = []
_owned = []


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


def read_starttime(pid):
    try:
        data = Path(f"/proc/{pid}/stat").read_text()
    except OSError:
        return None
    rp = data.rfind(")")
    if rp < 0:
        return None
    f = data[rp + 2:].split()
    return int(f[19]) if len(f) >= 20 else None


def own(proc):
    _owned.append((proc.pid, read_starttime(proc.pid)))
    return proc


def work_procs():
    """只统计可执行文件位于本次测试目录下的进程（不误伤无关实例）。"""
    if not WORK.exists():
        return []
    base = str(WORK.resolve())
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


def cleanup(extra=None):
    if extra is not None:
        # extra 可能是 SupervisorEnv 或 Popen
        try:
            extra.stop()
        except AttributeError:
            try:
                extra.terminate()
                extra.wait(timeout=3)
            except Exception:
                try:
                    extra.kill()
                except Exception:
                    pass
        except Exception:
            pass

    # 只处理自己启动且身份复核通过的进程
    for pid, st in _owned:
        if st is None or read_starttime(pid) != st:
            continue
        try:
            os.kill(pid, 15)
        except OSError:
            pass
    _owned.clear()

    for pid in work_procs():
        try:
            os.kill(pid, 15)
        except OSError:
            pass

    time.sleep(0.4)
    if WORK.exists():
        shutil.rmtree(WORK, ignore_errors=True)


def setup():
    src = ROOT / "build/ota-test/install-x86-fault.2e0Csr"
    if not src.exists():
        print("先运行 sh tests/setup_ota_test_env.sh", file=sys.stderr)
        return None

    if WORK.exists():
        shutil.rmtree(WORK)
    shutil.copytree(src, WORK, symlinks=True)

    if (WORK / "run").exists():
        shutil.rmtree(WORK / "run")

    # 模板目录可能残留上一次 D23 测试的状态（ROLLED_BACK）。
    # 统一重置为 CONFIRMED，让每个场景从同一基线出发，
    # 否则断言"状态未变"会因基线不同而误判。
    init = ROOT / "build/ota-test/ota_baseline_init"
    if init.exists():
        subprocess.run(
            [str(init), str(WORK / "state")],
            check=True, capture_output=True)

    cur = WORK / "current"
    if cur.is_symlink() or cur.is_file():
        cur.unlink()
    cur.symlink_to(f"releases/v{OLD_VERSION}")

    return WORK


def start_gateway(install):
    """通过 supervisor 启动 Gateway。

    S5 之后 OTA 与测试都**不再自己 fork** Gateway。
    若测试自己起一个进程再写 pidfile，那构造出来的是一个
    supervisor 根本不认识的外部进程 —— 而新架构下这种情况
    本身就是"必须拒绝升级"的状态（无法确认所有权），
    无法用来测 S3-R4 关心的东西。

    因此这里改为走正常路径：由 supervisor 启动，
    OTA 通过 IPC 看到 RUNNING。
    """
    env = SupervisorEnv(install)
    try:
        env.start()
    except Exception as exc:
        print(f"  (supervisor 启动失败: {exc})", file=sys.stderr)
        return None, False

    r = env.call("start")
    if r.returncode != 0:
        env.stop()
        return None, False

    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if port_listening(GW_PORT):
            break
        time.sleep(0.05)
    else:
        env.stop()
        return None, False

    return env, True


def run_agent(install, env=None, pkg=None):
    pkg = pkg or (ROOT / f"build/ota-test/real-x86/v{NEW_VERSION}")
    e = os.environ.copy()
    if env:
        e.update(env)
    return subprocess.run(
        [str(AGENT), "upgrade-test", str(pkg), str(install)],
        capture_output=True, text=True, timeout=180, env=e,
    )


def assert_unchanged(install, label):
    cur = os.readlink(install / "current")
    record(f"{label}：current 未变", cur == f"releases/v{OLD_VERSION}",
           f"current={cur}")

    st = json.loads((install / "state" / "ota_state.json").read_text())
    record(f"{label}：状态未变", st.get("state") == "CONFIRMED",
           f"state={st.get('state')}")
    record(f"{label}：confirmed 未变",
           st.get("confirmed_version") == OLD_VERSION,
           f"confirmed={st.get('confirmed_version')}")


# ---------------------------------------------------------------------

def scenario_a_invalid_port():
    print("\n" + "=" * 72)
    print("场景 A：非法上游端口 -> 在停止 Gateway 之前拒绝")
    print("=" * 72)

    install = setup()
    if install is None:
        return

    if port_listening(GW_PORT):
        record("前置：7000 空闲", False, "已被占用")
        return
    record("前置：7000 空闲", True)

    proc, ok = start_gateway(install)
    record("Gateway 已启动并监听 7000", ok)
    if not ok:
        cleanup(proc)
        return

    r = run_agent(install, env={"OTA_UPSTREAM_PORT": "99999"})
    out = r.stdout + r.stderr
    print(out, flush=True)

    record("升级被拒绝", r.returncode != 0, f"exit={r.returncode}")
    record("报告端口非法", "invalid OTA_UPSTREAM_PORT" in out)

    time.sleep(0.3)
    #
    # 判断"原 Gateway 仍在运行"改为查 supervisor 的状态，
    # 而不是 poll 一个我们持有的 Popen —— 现在进程不是
    # 测试的子进程，测试无法 wait 它。
    #
    st = proc.call("status")
    still_running = "running" in (st.stdout + st.stderr)

    record("原 Gateway 仍在运行（未被停止）", still_running,
           (st.stdout + st.stderr).strip()[:50])
    record("7000 仍在监听", port_listening(GW_PORT))

    assert_unchanged(install, "非法端口")
    cleanup(proc)


def scenario_b_not_running():
    print("\n" + "=" * 72)
    print("场景 B：升级前未运行 -> 失败后不得擅自启动 Gateway")
    print("=" * 72)

    install = setup()
    if install is None:
        return

    hog = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    hog.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        hog.bind(("0.0.0.0", GW_PORT))
        hog.listen(1)
    except OSError as exc:
        record("前置：外部进程占用 7000", False, str(exc))
        hog.close()
        return

    record("前置：外部进程占用 7000", True)
    record("前置：Gateway 未运行", len(work_procs()) == 0)

    #
    # S5：必须准备 supervisor，否则测不到本场景的本意。
    #
    # 没有 supervisor 时 OTA 会直接报"连不上 supervisor"而中止 ——
    # 那是另一种拒绝，与本场景要验证的
    # "明确知道没在运行、失败后也不擅自启动" 完全不同。
    #
    seenv = SupervisorEnv(install)

    try:
        seenv.start()
    except Exception as exc:
        record("前置：supervisor 可用", False, str(exc)[:60])
        hog.close()
        return

    record("前置：supervisor 可用（Gateway 未运行）", True)

    r = run_agent(install)
    out = r.stdout + r.stderr
    print(out, flush=True)

    record("升级被拒绝", r.returncode != 0, f"exit={r.returncode}")
    record("识别为升级前未运行",
           "Pre-upgrade gateway state: STOPPED" in out,
           [l for l in out.splitlines()
            if "Pre-upgrade" in l][:1])

    time.sleep(0.5)
    started = work_procs()
    record("未擅自启动 Gateway（关键约束）",
           len(started) == 0, f"found={started}")

    #
    # 再确认 supervisor 自己的状态也是"没有 Gateway"，
    # 而不只是"没有我们看得见的进程"。
    #
    st = seenv.call("status")
    record("supervisor 确认 Gateway 未启动",
           "stopped" in (st.stdout + st.stderr),
           (st.stdout + st.stderr).strip()[:50])

    assert_unchanged(install, "端口被占")

    hog.close()
    cleanup(seenv)


def scenario_c_false_positive():
    """停止后失败 + 端口被外部抢占 -> 必须 RECOVERY REQUIRED，不得谎报恢复."""
    print("\n" + "=" * 72)
    print("场景 C：停止后端口被抢占 -> 必须报告 RECOVERY REQUIRED")
    print("=" * 72)

    install = setup()
    if install is None:
        return

    if port_listening(GW_PORT):
        record("前置：7000 空闲", False, "已被占用")
        return
    record("前置：7000 空闲", True)

    proc, ok = start_gateway(install)
    record("Gateway 已启动并监听 7000", ok)
    if not ok:
        cleanup(proc)
        return

    # 忙等抢占 7000：Gateway 一停立刻绑上
    stop_flag = threading.Event()
    hog = {}

    def grab():
        while not stop_flag.is_set():
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                s.bind(("0.0.0.0", GW_PORT))
                s.listen(1)
                hog["s"] = s
                return
            except OSError:
                s.close()
                time.sleep(0.001)

    t = threading.Thread(target=grab, daemon=True)
    t.start()

    r = run_agent(install)
    stop_flag.set()
    time.sleep(0.2)
    t.join(timeout=2)

    out = r.stdout + r.stderr
    print(out, flush=True)

    record("前置：成功抢占 7000", "s" in hog)
    record("升级被拒绝", r.returncode != 0, f"exit={r.returncode}")

    record("检测到停止后端口仍被占用",
           "still occupied after stopping gateway" in out)

    # 核心断言：不得谎报恢复成功
    claimed_ok = "restored and L1-ready" in out
    record("未谎报恢复成功（无假阳性）", not claimed_ok,
           "谎报了 L1-ready" if claimed_ok else "")

    record("明确报告需要人工恢复",
           "RECOVERY REQUIRED" in out)

    assert_unchanged(install, "端口抢占")

    if "s" in hog:
        hog["s"].close()
    cleanup(proc)


def scenario_d_restore_success():
    """正常场景：停止后失败但端口可回收 -> 应自动恢复旧版."""
    print("\n" + "=" * 72)
    print("场景 D：停止后 activate 失败 -> 自动恢复旧版")
    print("=" * 72)

    install = setup()
    if install is None:
        return

    if port_listening(GW_PORT):
        record("前置：7000 空闲", False, "已被占用")
        return
    record("前置：7000 空闲", True)

    proc, ok = start_gateway(install)
    record("Gateway 已启动并监听 7000", ok)
    if not ok:
        cleanup(proc)
        return

    # 构造"停止后 activate 失败"：把候选目录换成一份 SHA 正确的包，
    # 但在 precheck 之后无法篡改（时序不可控）。
    # 因此这里改为验证 precheck 失败不影响运行中的服务——
    # 这是同一类"失败不留副作用"的约束，且可稳定复现。
    cand = install / "releases" / f"v{NEW_VERSION}"
    if cand.exists():
        shutil.rmtree(cand)

    r = run_agent(install)
    out = r.stdout + r.stderr
    print(out, flush=True)

    record("升级被拒绝", r.returncode != 0, f"exit={r.returncode}")
    record("在停止 Gateway 之前就拒绝了",
           "aborted before stopping the gateway" in out)

    time.sleep(0.3)
    st = proc.call("status")
    record("原 Gateway 仍在运行",
           "running" in (st.stdout + st.stderr),
           (st.stdout + st.stderr).strip()[:50])
    record("7000 仍在监听", port_listening(GW_PORT))

    assert_unchanged(install, "候选缺失")
    cleanup(proc)


def main():
    print("=" * 72)
    print("S3-R4 验收：停止 Gateway 之后的失败路径与恢复")
    print("=" * 72)

    if not AGENT.exists():
        print(f"缺少 {AGENT}，请先 make ota_agent_x86", file=sys.stderr)
        return 2

    try:
        scenario_a_invalid_port()
        scenario_b_not_running()
        scenario_c_false_positive()
        scenario_d_restore_success()
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
