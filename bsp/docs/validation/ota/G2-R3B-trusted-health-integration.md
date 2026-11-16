# G2-R3B 可信健康集成：主机增量验收

## 结论与边界

**G2-R3B HOST-ACCEPTED：BLOCKED；G1/G2 板端验收：BLOCKED。** 本轮完成的是主机诊断增量，不是可信系统 OTA 确认闭环。正常确认路径 N09 未完成、未运行；生产 `sgw_bootctl confirm` 继续默认拒绝，不接受命令行健康布尔值或测试环境变量放行。没有刷写、修改板端 eMMC/U-Boot 环境、启用 Watchdog、操作正式 Gateway 或推送远程。

本轮 HOST-TESTED 仅指具体已通过的隔离测试，不等于全部场景通过。BOARD-TESTED：本轮全部 NOT RUN。G2-R4 可以进行离线设计及隔离研究，但不能据此部署自动确认/Watchdog 或进入板端启动、回滚试验。G1 的独立启动、离线备份、SHA256 及恢复验证仍须另行完成。

## 基线、历史证据与复用

- 2026-11-16，Ubuntu VM 本地 `~/Desktop/project/SmartGateway-BSP`，分支 `feature/imx6ull-system-ab-ota`；改动前 HEAD 为 `bf2ea456f4660cadf04bd02e988455da597af318`，历史包含 `8bd2a845` 和 `cfffd69`，工作区干净。远程跟踪核对值 `60691df5db73c72e6c5a4f206408d5fe184b612b`，本轮不推送。隔离副本位于 VM 的临时目录，最终改动作为独立本地提交保留；提交后的 SHA 以 Git 记录为准。
- 历史 `feature/bsp-imx6ull` 本地和远程跟踪 HEAD 为 `b1620ca67784318c355115c2065a76d39ca4973f`。指定文档已整理到 `bsp/docs/validation/`，未合并旧分支。

| 历史资料和代码 | 可复用证据 | 边界 |
|---|---|---|
| [D06/D07](../gpio/D06-gpio-end-to-end-validation.md)、[D07](../gpio/D07-gpio-reliability-validation.md) | 历史板端 GPIO 驱动及事件链路测试 | 长期稳定性未全部实测，D07 有报告引用反馈；不能证明本次代码已板测 |
| [D12–D15](../sensors/ap3216c/D12-ap3216-fault-injection-lifecycle.md)、[D13](../sensors/ap3216c/D13-ap3216-i2c-error-handling.md)、[D14](../sensors/ap3216c/D14-ap3216-stats-ratelimited-log.md)、[D15](../sensors/ap3216c/D15-ap3216c-sensor-agent-integration.md) | 既有 I²C 故障、统计、SENSOR 语义；ALS 0 为有效值 | 是历史固件/驱动证据，不能代替本次健康检查板测 |
| [D25-1B](../build/D25-1B-board-environment-recheck.md) | 既有板端 `/opt` 跨重启记录及工具链信息 | 当前 `/opt` 在 p2；重启持久不证明跨槽共享 |
| [S5](supervisor/D25-2-S5-service-supervisor.md)、[S6–S7](supervisor/D25-2-S6-S7-board-upgrade-and-recovery.md) | 应用 OTA 和 Supervisor 历史行为 | 不能证明系统 A/B；其中断电叙述可见命令为 sync + reboot，不能据此认定真实物理掉电 |
| `ota_health.c`、`ota_upgrade_run.c`、`tests/test_ota_health_e2e.py` | 复核 TCP L1 与 OTA01 真实 L2 转发；本次隔离测试重复使用已有测试 | 旧 L2 需要配置专用上游，会注册节点/产生业务消息，不能原样用于生产只读检查 |
| `ota_state_io.c`、`ota_oplog.c`、`ota_lock.c`、`ota_upgrade_run.c` | 审核原子状态记录、应用事务和互斥；复跑状态读写 | 只覆盖应用 OTA；尚未与共享 DATA、系统 OTA 事务或确认联动 |
| `service_supervisor/`、`tests/test_s5_supervisor_e2e.py`、G2-R3 `health_supervisor.c` | 复用 IPC、进程身份和生命周期检查；隔离回归 | 历史应用 OTA 成功不等同系统 A/B 成功 |

## 本轮代码与安全边界

1. `gateway/health_probe.[ch]`、`gateway/gateway_loop.c`、顶层 `Makefile`：Gateway 主循环可选监听仅本机 Unix socket，使用随机挑战、对端身份及访问权限限制。查询经过真实 DATA 解码/编码和当前队列、设备管理器的非修改检查，不注册节点、不写业务队列、不修改上游。监听不可用时 Gateway 继续原流程，健康检查失败关闭。普通 DATA 转发有隔离回归。该检查**没有穿过完整入站/排队/上游转发路径**，不能证明 Gateway 完整业务健康；故诊断仍输出 `gateway_business=UNKNOWN`。
2. `health_gateway.[ch]`、`health_supervisor.[ch]`：复用 Supervisor IPC，核验 Gateway PID、启动时间及 Unix socket 的 `SO_PEERCRED`，有超时及挑战校验；Gateway 检查前后再次核对 Supervisor。独立测试编译宏允许测试路径，正式构建不含该环境变量覆盖入口。
3. `health_devices.[ch]`：检查 GPIO 字符设备身份、平台驱动绑定及只读 stats；不读取事件队列，IRQ 为零可正常。AP3216C 检查驱动绑定、`als_raw` 和统计，ALS 0 有效、硬件 I/O 失败为不健康；心跳未接入时即使直接读取成功仍为 UNKNOWN，`SENSOR=ERROR` 优先判不健康。读取由限时子进程隔离，但不可中断内核 I/O 可能使子进程暂时无法回收；这不是可直接部署的周期服务。
4. `health_network.[ch]`：从受保护的固定策略文件 `/etc/smartgateway/system-health-network.conf` 读取 `interface=<接口名>`，查询接口、链路与非回环地址；无配置、无可信状态为 UNKNOWN。未部署策略，也未验证完整本地业务通道；不写死测试 IP。外部不可达仅在现有四态模型中覆盖 DEGRADED，真实外网状态未接入。
5. `sgw_system_health.c` 是只读一次性诊断：DATA、完整 Gateway 业务及不可证明项保持 UNKNOWN；真实缺失必需设备可显示 UNHEALTHY；`confirmation=DISABLED`。未接入生产确认或 Watchdog。测试：`tests/health_devices_test.c`、`tests/health_network_test.c`、`tests/gateway_probe_cli.c`、`tests/supervisor_probe_cli.c`、根目录 `tests/test_g2r3_health.py` 和 `tests/test_g2r3b_gateway_probe.py`；构建规则和忽略项同步更新。

已知工程限制：socket 由 root 持有且仅 root 可访问；单次客户端输入最长占用 Gateway 主循环约 200 ms，需后续做负载及滥用评估。异常退出留下旧 socket 时不会擅自删除，可能令诊断不可用，安全上保持拒绝，需设计安全恢复。设备固定路径、网络策略和产品必需链路均需板端核实。`fw_env` 现有后端还有子进程超时问题，不能将诊断服务视作具备生产可用性。

## 增量验收矩阵

日志均为 HOST 隔离运行，命令与日志在下一节；未列板端结果。

| ID | 实际覆盖与证据 | 结果 |
|---|---|---|
| N01 | 真实隔离 Supervisor + Gateway：组件查询成功、上游无合成消息；普通 DATA 独立验证可转发。原 L2 测试在独立 Gateway/上游成功，但该 L2 不满足生产无副作用条件 | PARTIAL HOST-TESTED；完整无副作用业务探针 BLOCKED |
| N02 | 隔离 Gateway 用 SIGSTOP 停止，PID 仍存活，查询拒绝；清理时 SIGCONT | HOST-TESTED PASS |
| N03 | 设备解析和语义 21 项：ALS 0、格式、SENSOR ERROR、GPIO 零 IRQ、超时；主机无设备时不判健康 | PARTIAL HOST-TESTED；缺完整可注入设备/I²C 故障与板端验证 |
| N04 | 接口无配置、非法、loopback、不存在检查；模型区分外网 DEGRADED | PARTIAL HOST-TESTED；真实策略与本地业务链路、外网状态未联动 |
| N05 | 未实现共享 DATA 身份、空间、schema 及写权限检查 | BLOCKED / NOT RUN |
| N06 | 未实现跨系统/应用 OTA 事务的可信集成检查 | BLOCKED / NOT RUN |
| N07 | 现有模型测试槽位/实例等不匹配；无可信包与镜像摘要绑定、生产事务 | PARTIAL；最终确认门控 BLOCKED |
| N08 | 现有健康模型检查观察中断/失败后重新累计 | HOST-TESTED PASS（模型） |
| N09 | 真实 Gateway + DATA + 系统事务 + 生产一致确认决策的成功链不存在 | BLOCKED / NOT RUN；不得用内存模型冒充成功 |
| N10 | 进程身份/启动时间不符、停止、超时及未知拒绝；未形成持久确认后的重放、幂等、落盘失败闭环 | PARTIAL；完整场景 BLOCKED |

## 主机测试命令、结果和日志

在隔离副本 `/tmp/sgw-g2r3b-GCn1u7/repo` 执行；日志为本次原始输出。G2 U-Boot 仿真指向已有独立主机构建目录 `/tmp/sgw-g1g2-audit-c2TLcv/repo/build/uboot-ab`，不访问真实设备。

| 命令 | 实测结果 | 日志 |
|---|---|---|
| `make -C bsp/apps/boot_ab test arm-check` | ab_state 100/100，冗余环境 31/31，ab_health FAIL=0，健康模型 195/195，设备 21/21，网络 5/5；ARM 构建通过 | [boot.log](g2-r3b-host-logs/boot.log) |
| `UBOOT_AB_BUILD_DIR=/tmp/sgw-g1g2-audit-c2TLcv/repo/build/uboot-ab python3 -m unittest -v tests.test_g1_backup_guard tests.test_g2_uboot_ab tests.test_g2r3_health` | 25/25 | [regression-rerun.log](g2-r3b-host-logs/regression-rerun.log) |
| `make -C bsp/apps/service_supervisor test` | 58/58、35/35 | [supervisor.log](g2-r3b-host-logs/supervisor.log) |
| `make -C bsp/apps/ota_agent test-supervisor test-supervisor-s2 HOST_SUP_BIN=/tmp/sgw-g2r3b-GCn1u7/ota-sup-test HOST_S2_BIN=/tmp/sgw-g2r3b-GCn1u7/ota-sup-s2-test` | 145/145、44/44 | [ota.log](g2-r3b-host-logs/ota.log) |
| `gcc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror ota_state_io_test.c ota_state_io.c ota_state.c -ljson-c -o /tmp/sgw-g2r3b-GCn1u7/ota-state-test`，再对独立临时目录运行 | 3 组状态读写测试 PASS；不证明跨槽 DATA | [ota-state.log](g2-r3b-host-logs/ota-state.log) |
| `make test` | 10 个 C 单元测试程序 PASS | [core.log](g2-r3b-host-logs/core.log) |
| `make gateway; make arm; make arm-check` | Gateway 主机和 ARM 构建、ABI 检查通过 | [gatewaybuild.log](g2-r3b-host-logs/gatewaybuild.log)、[abi.log](g2-r3b-host-logs/abi.log) |
| `make gateway BUILD_DIR=build/r3b-gateway CFLAGS="-Wall -Wextra -Werror -std=c11 -pthread -DSGW_GATEWAY_HEALTH_TESTING"`；`make -C bsp/apps/service_supervisor all`；`python3 -m unittest -v tests.test_g2r3b_gateway_probe` | 两项真实隔离进程集成测试 PASS | [gateway-probe-build.log](g2-r3b-host-logs/gateway-probe-build.log)、[gateway-probe-final.log](g2-r3b-host-logs/gateway-probe-final.log) |
| `timeout 40 python3 tests/test_ota_health_e2e.py` | 旧 L2 独立 Gateway 和临时上游 PASS，非生产无副作用探针 | [ota-health-e2e.log](g2-r3b-host-logs/ota-health-e2e.log) |
| 先确认 TCP 7000 可绑定，再运行 `timeout 120 python3 tests/test_s5_supervisor_e2e.py` | 33/33；进程启停和 SIGKILL 仅针对隔离测试进程 | [s5-e2e.log](g2-r3b-host-logs/s5-e2e.log) |

初次并行运行构建和 Python 回归时，构建正在替换诊断程序，出现一次 `PermissionError`；随后顺序重跑 25/25，保留[初次失败日志](g2-r3b-host-logs/regression.log)。手工编译应用状态测试时首次漏写 `-ljson-c`，链接失败；补全后通过，属于测试命令错误，不是产品功能通过证据。日志中 S5 的“FAIL: another supervisor...”是测试预期的第二实例拒绝，最终 33/33。

ARM 使用 Buildroot SDK，Gateway 为 ELF32 ARMv7 hard-float，动态解释器 `/lib/ld-linux-armhf.so.3`，依赖 `libpthread.so.0`、`libc.so.6`，所需最高 glibc 符号为 2.28；bootctl、watchdog guard、诊断程序均 ARM ELF32、依赖 libc、最高 glibc 符号 2.28。历史板端记录 glibc 2.30，但此次未部署或验证板端运行。正式主机 Gateway 二进制检查不到测试路径覆盖变量；该检查不代替生产发行物审计。

## 下一步最小整改和放行条件

1. 冻结并验证独立共享 DATA 架构：设备身份与挂载拓扑、空间、权限、兼容 schema、应用/系统事务和互斥；独立镜像/目录测试槽位切换及损坏恢复。不得把当前 p2 的 `/opt` 当作已验证共享 DATA。
2. 由可信升级者生成不可预测事务 ID，并持久绑定升级包和镜像摘要、目标槽、last-good；把当前真实 root、boot_id、稳定观察、DATA 与所有必需探针绑定到同一启动实例。实现抗重放、过期、进程替换、安全幂等及持久化失败拒绝，并用与生产相同的决策代码完成 N05–N10；任何 UNKNOWN 仍拒绝。
3. 完成不污染生产数据的真实 Gateway 业务路径、可信 SENSOR 心跳、GPIO/I²C/本地网络接口和超时测试；验证负载和生命周期。N09 必须经过实际 Gateway 业务集成路径。
4. 仅在上述主机门槛通过后开展 G2-R4 的主机 Watchdog 接管实验；真实上板前完成 G1 独立介质启动、离线完整备份、SHA256 和恢复验证，核验恢复环境 Python 3 等依赖。保护当前 p2，并先核实槽位空间、U-Boot 环境和 bootcount 的持久化后端与掉电恢复。
5. 板端后续必须单独验证 p2 保底、候选启动、持久 bootcount、Watchdog 交接、altbootcmd 自动回滚及受控掉电；须遵守用户另行授权和恢复放行条件。本轮这些证据均不存在。
