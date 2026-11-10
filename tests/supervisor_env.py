#!/usr/bin/env python3
"""测试辅助：为 OTA 集成测试提供 Service Supervisor 环境

背景（为什么需要这个模块）
--------------------------
S5 之前 OTA 自己 fork Gateway 并维护 pidfile，集成测试只要准备
一个安装目录就够了。

S5 之后 Gateway 由常驻 supervisor 管理（这是解决 L3/L6 的唯一
可行方式），OTA 通过 Unix Domain Socket 与它通信。
因此任何跑完整升级流程的测试，都必须先有一个 supervisor
在监听。

这个模块把"准备 supervisor 环境"固化下来，避免每个测试脚本
各写一遍，也避免各自实现出细微差别（那会变成最难查的一类问题）。

设计要点
--------
1. **每个安装目录一个 supervisor**
   socket 放在 <install>/run/ 下，因此测试之间天然隔离，
   可以并行而不互相干扰。

2. **supervisor 的 executable 指向 <install>/current/smart_gateway**
   这是软链。Gateway 被 OTA 切换 current 之后，下次 start
   自然启动新版——与生产行为一致，不需要测试特殊处理。

3. **必须等待 socket 真正可用才返回**
   只等进程存在是不够的：supervisor 可能因配置错误立刻退出。
   一个"进程在但 socket 连不上"的环境会让后续测试全部超时，
   而原因看起来像是 OTA 的问题。

用法
----
    from supervisor_env import SupervisorEnv

    env = SupervisorEnv(install_dir)
    try:
        env.start()
        # ... 跑 OTA 升级 ...
    finally:
        env.stop()
"""

import os
import shutil
import signal
import socket
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SUPERVISOR = ROOT / "bsp/apps/service_supervisor/service_supervisor"


def _read_stat(pid):
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


def find_procs(exe_suffix=None, exe_prefix=None):
    """按 /proc/<pid>/exe 查找进程。

    不用 ps/pgrep：BusyBox 的 ps 输出格式与 GNU 不同，
    且 `ps | grep -c` 会把 grep 自己算进去（项目里踩过这个坑）。
    """
    found = []
    for e in Path("/proc").iterdir():
        if not e.name.isdigit():
            continue
        try:
            exe = os.readlink(e / "exe").replace(" (deleted)", "")
        except OSError:
            continue
        if exe_suffix and exe.endswith(exe_suffix):
            found.append((int(e.name), exe))
        elif exe_prefix and exe.startswith(exe_prefix):
            found.append((int(e.name), exe))
    return found


class SupervisorEnv:
    """为一个安装目录管理 supervisor 的生命周期。"""

    def __init__(self, install_dir, executable=None,
                 server="127.0.0.1", port=9000,
                 stop_timeout_ms=5000, verbose=False):
        self.install = Path(install_dir).resolve()
        self.run_dir = self.install / "run"
        self.sock = self.run_dir / "supervisor.sock"
        self.conf = self.install / "service.conf"
        self.log = self.install / "supervisor.log"
        self.proc = None
        self.verbose = verbose

        # 默认让 supervisor 启动 <install>/current/smart_gateway。
        # current 是软链，切换后自动指向新版——与生产一致。
        self.executable = executable or str(
            self.install / "current" / "smart_gateway")

        self.server = server
        self.port = port
        self.stop_timeout_ms = stop_timeout_ms

    # ------------------------------------------------------------

    def write_config(self):
        self.run_dir.mkdir(parents=True, exist_ok=True)
        self.conf.write_text(
            f"executable={self.executable}\n"
            f"serial=\n"
            f"server={self.server}\n"
            f"port={self.port}\n"
            f"stop_timeout_ms={self.stop_timeout_ms}\n")

    def start(self, timeout=10):
        """启动 supervisor 并等待 socket 可用。"""
        if not SUPERVISOR.exists():
            raise RuntimeError(
                f"缺少 {SUPERVISOR}\n先运行: make supervisor")

        self.write_config()

        # 清掉可能残留的状态，让每次测试从确定的起点开始
        for p in (self.run_dir / "service.pid", self.sock):
            if p.exists() or p.is_symlink():
                p.unlink()

        self.log.parent.mkdir(parents=True, exist_ok=True)
        logf = open(self.log, "wb")

        self.proc = subprocess.Popen(
            [str(SUPERVISOR), "run", "--root", str(self.install)],
            stdout=logf, stderr=subprocess.STDOUT,
            start_new_session=True)
        logf.close()

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.sock.exists() and self._probe():
                return self
            if self.proc.poll() is not None:
                raise RuntimeError(
                    f"supervisor 启动失败 (exit={self.proc.returncode})\n"
                    f"日志: {self.log.read_text()[-800:]}")
            time.sleep(0.05)

        raise RuntimeError(
            f"supervisor 在 {timeout}s 内未就绪\n"
            f"日志: {self.log.read_text()[-800:]}")

    def _probe(self):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(1.0)
            s.connect(str(self.sock))
            s.sendall(b"STATUS\n")
            data = s.recv(256)
            s.close()
            return b"\n" in data
        except OSError:
            return False

    def call(self, request, timeout=30):
        r = subprocess.run(
            [str(SUPERVISOR), request, "--root", str(self.install)],
            capture_output=True, text=True, timeout=timeout)
        if self.verbose:
            print(f"$ supervisor {request}\n{r.stdout}{r.stderr}")
        return r

    def stop(self):
        """停止 supervisor 与它启动的 Gateway。

        清理必须彻底。曾经出现过一批 supervisor 泄漏：
        场景测试中途失败（未走到 stop）时，这些进程一直留着，
        后续测试的 socket 冲突、端口占用都由此而来，
        而症状看起来像是被测代码的问题。

        因此这里除了优雅停止，还有两道兜底。
        """
        if self.sock.exists():
            try:
                self.call("stop", timeout=30)
            except subprocess.TimeoutExpired:
                pass
            except Exception:
                pass

        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                try:
                    self.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass

        self.proc = None

        #
        # 兜底一：本安装目录下的 Gateway。
        #
        # 注意用 exe_prefix 匹配**本目录**，不误伤其它测试。
        # 顽固进程可能忽略 SIGTERM，所以直接 SIGKILL。
        #
        for pid, _ in find_procs(exe_prefix=str(self.install)):
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass

        #
        # 兜底二：绑定本安装目录的 supervisor 进程。
        #
        # 不能只靠 self.proc：测试可能在 start() 失败后
        # 直接抛异常，此时 self.proc 可能为 None，
        # 也可能与实际存活的进程不是同一个（例如 start 内部
        # 重建过）。用命令行参数里的 --root 精确匹配。
        #
        for pid, _ in find_procs(exe_suffix="/service_supervisor"):
            try:
                cmdline = Path(f"/proc/{pid}/cmdline").read_bytes()
            except OSError:
                continue

            args = [a for a in cmdline.split(b"\0") if a]

            if b"run" not in args:
                continue

            if str(self.install).encode() not in args:
                continue

            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass

        time.sleep(0.2)


def install_root(name, src_install, work_root):
    """把模板安装目录复制到独立位置，供一个测试独占使用。

    为什么不直接在 build/ota-test/install-x86-fault.* 上跑：
    那是 setup_ota_test_env.sh 的产物，多个测试共用会互相
    干扰（pidfile、current、run/ 目录都是共享状态）。
    """
    dst = Path(work_root) / name
    if dst.exists():
        shutil.rmtree(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(src_install, dst, symlinks=True)

    # run/ 里可能有上个使用者的 supervisor.sock，必须清掉
    if (dst / "run").exists():
        shutil.rmtree(dst / "run")

    (dst / "last_operation.json").unlink(missing_ok=True)
    (dst / "supervisor.log").unlink(missing_ok=True)

    return dst
