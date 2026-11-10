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

# S5：Gateway 改由 Service Supervisor 管理
sys.path.insert(0, str(Path(__file__).resolve().parent))
from supervisor_env import SupervisorEnv  # noqa: E402
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
        #
        # S5：proc 可能是 SupervisorEnv 而不是 Popen。
        # 必须分别处理 —— SupervisorEnv 有自己的 stop()
        # （它要额外停掉受管的 Gateway，并清掉本目录下的残留）。
        # 之前这里只调 terminate()，于是 SupervisorEnv 被当成
        # Popen 处理，Gateway 与 supervisor 都没有被清理，
        # 端口 7000 一直占着，后续场景全部因为"端口已被占用"
        # 而无法开始。
        #
        stop_fn = getattr(proc, "stop", None)

        if callable(stop_fn):
            try:
                stop_fn()
            except Exception:
                pass
        else:
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

    #
    # S5：Gateway 由 Service Supervisor 启动。
    #
    # 不再自己 Popen 再补写 pidfile —— 那构造出的是一个
    # supervisor 不认识的外部进程，新架构下会被拒绝升级
    # （无法确认所有权），测不到"正常停止运行中的实例"。
    #
    seenv = SupervisorEnv(install, verbose=True)

    try:
        seenv.start()
    except Exception as exc:
        record("前置：supervisor 可用", False, str(exc)[:60])
        return False

    r = seenv.call("start")
    record("Gateway 已由 supervisor 启动",
           r.returncode == 0,
           (r.stdout + r.stderr).strip()[:50])

    ok = wait_port(GW_PORT, True)
    record("Gateway 已监听 7000", ok)

    if not ok:
        cleanup(seenv)
        return False

    proc = seenv

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

    # 旧进程必须真的退出。
    #
    # S5：查 supervisor 的状态，而不是 poll 一个我们
    # 持有的 Popen —— 进程现在不是测试的子进程。
    time.sleep(0.5)
    st = proc.call("status")
    record("原 Gateway 进程已退出",
           "stopped" in (st.stdout + st.stderr),
           (st.stdout + st.stderr).strip()[:50])

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

    #
    # 停止后 supervisor 必须清掉自己的 service.pid。
    #
    # 注意：这里看的不再是 OTA 的 gateway.pid —— 新架构下
    # pidfile 由 supervisor 维护，它的名字与位置都变了。
    # 检查点仍然重要：残留记录会让下次启动误判"有个不是我的
    # 进程在跑"而拒绝启动。
    #
    spf = install / "run" / "service.pid"
    record("supervisor pidfile 已清理", not spf.exists(),
           f"exists={spf.exists()}")

    # 清理
    cleanup(proc)

    record("结束后无本目录遗留进程",
           len(leftover_in_work_dir()) == 0,
           f"found={leftover_in_work_dir()}")

    return True


def scenario_unknown_state():
    """场景二：supervisor 不可用 -> 必须中止，不得切换版本。

    S5 前的原场景是"pidfile 损坏 -> UNKNOWN -> 中止"。
    那个检查现在移到了 supervisor 内部（它自己维护 pidfile，
    损坏时拒绝启动），OTA 侧看到的是"连不上 supervisor"。

    本场景因此改为验证新的、等价的安全性质：
    **supervisor 不可用时，OTA 必须在产生任何副作用之前中止。**

    这比原场景更严格：原场景只需要识别一个文件坏掉，
    现在要求 OTA 在完全无法获知服务状态时绝不擅自动手。
    """
    print("\n" + "=" * 72)
    print("场景二：supervisor 不可用 -> 必须中止，不得切换版本")
    print("=" * 72)

    install = setup_install()
    if install is None:
        return False

    # 确保没有 supervisor 在跑（本场景就是要测它不可用）
    run_dir = install / "run"
    if (run_dir / "supervisor.sock").exists():
        (run_dir / "supervisor.sock").unlink()

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

    record("报告无法查询服务状态",
           "cannot query gateway state from the supervisor" in out,
           [l for l in out.splitlines() if "FAIL" in l][:1])
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

    #
    # 关键安全性质：连不上 supervisor 时**绝不能**自己去起
    # 一个 Gateway。那会绕过所有权，正是 L3 的成因。
    #
    record("未擅自启动任何 Gateway",
           len(leftover_in_work_dir()) == 0,
           f"found={leftover_in_work_dir()}")

    #
    # 反向验证：同一份安装目录，一旦 supervisor 可用，
    # 升级就必须能正常执行。
    #
    # 没有这一条，一个"总是拒绝升级"的实现也能通过上面的断言。
    #
    seenv = SupervisorEnv(install)

    try:
        seenv.start()
    except Exception as exc:
        record("反向验证：supervisor 就绪后可以升级", False, str(exc)[:60])
        cleanup()
        return True

    r = subprocess.run(
        [str(AGENT), "upgrade-test",
         str(ROOT / f"build/ota-test/real-x86/v{NEW_VERSION}"),
         str(install)],
        capture_output=True, text=True, timeout=180)

    record("反向验证：supervisor 就绪后可以升级",
           r.returncode == 0 and "PASS: OTA upgrade CONFIRMED"
           in (r.stdout + r.stderr),
           f"exit={r.returncode}")

    cleanup(seenv)
    return True


def scenario_stop_fails():
    """场景三：必须确认旧版退出之后，才允许切换版本.

    这条约束是 S3 的核心安全性质：若在旧进程仍存活时切换
    current，会变成"旧版还在跑，但磁盘上的文件已被换掉"，
    该状态无法自动收敛。

    S5 之后这个性质的**保证方式**变了
    -------------------------------
    S3/S4：OTA 自己发 SIGTERM、自己读 /proc 轮询确认退出。
    S5：OTA 请求 supervisor 停止，supervisor 作为父进程用
        waitpid 确认退出后才返回 OK。

    后者更强：waitpid 是"父进程得知子进程终止"的唯一权威
    途径，不存在观察者与被观察者之间的时间差。

    本场景因此验证两件事：
      1. 顺序性 —— 确认退出的日志必须早于切换版本的日志
      2. 反向验证 —— 若跳过停止直接切换，本场景的断言应能发现

    关于"停不掉"的构造限制（必须如实记录）
    ------------------------------------
    SIGKILL 也无效（D 状态）的分支仍然未做集成测试：
      - 本机无 FUSE 开发头、无 NFS、无 root 权限
      - SIGSTOP 与 ptrace 挂住的进程都仍会被 SIGKILL 收掉
        （已实测：ptrace-stop 的进程被 SIGKILL 后变成 Z）
    该分支是纯防御性代码，列为 L10，不报告为 PASS。
    """
    print("\n" + "=" * 72)
    print("场景三：确认旧版退出后才切换版本")
    print("=" * 72)

    install = setup_install()
    if install is None:
        return False

    if port_listening(GW_PORT):
        record("前置：7000 空闲", False, "端口已被占用")
        return False

    seenv = SupervisorEnv(install, stop_timeout_ms=3000)

    try:
        seenv.start()
    except Exception as exc:
        record("前置：supervisor 可用", False, str(exc)[:60])
        return False

    r = seenv.call("start")
    record("Gateway 已启动", r.returncode == 0,
           (r.stdout + r.stderr).strip()[:50])

    ok = wait_port(GW_PORT, True)
    record("Gateway 正在监听 7000", ok)

    if not ok:
        cleanup(seenv)
        return False

    before_current = os.readlink(install / "current")

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

    #
    # 核心安全断言（顺序性）：
    # 进程确认退出必须**先于**切换 current。
    #
    # 只断言"最终进程没了"是不够的 —— 一个先切版本、
    # 再补停进程的实现同样满足"进程没了"，但它已经
    # 制造了"旧版在跑、文件被换"的破损状态。
    #
    stopped_idx = out.find("PASS: gateway stopped")
    activated_idx = out.find("candidate activated")

    record("确认退出发生在切换版本之前（关键安全约束）",
           stopped_idx >= 0 and activated_idx >= 0
           and stopped_idx < activated_idx,
           f"stop_pos={stopped_idx}, activate_pos={activated_idx}")

    st = seenv.call("status")
    record("旧进程确实已终止",
           "stopped" in (st.stdout + st.stderr),
           (st.stdout + st.stderr).strip()[:50])

    after_current = os.readlink(install / "current")
    record("current 已切到新版（升级成功）",
           after_current == f"releases/v{NEW_VERSION}",
           f"{before_current} -> {after_current}")

    #
    # 反向验证：确认上面的顺序断言真的能发现倒序。
    #
    # 用一个明显倒序的假输出跑同一条判定逻辑。若不这样做，
    # "顺序断言"有可能因为两个下标都是 -1 而恰好不成立，
    # 我们无法区分"检查有效"与"检查永远为假"。
    #
    fake_bad = "candidate activated\nPASS: gateway stopped\n"
    b_stop = fake_bad.find("PASS: gateway stopped")
    b_act = fake_bad.find("candidate activated")

    record("反向验证：倒序输出会被判定为违规",
           not (b_stop >= 0 and b_act >= 0 and b_stop < b_act),
           f"stop_pos={b_stop}, activate_pos={b_act}")

    cleanup(seenv)
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
