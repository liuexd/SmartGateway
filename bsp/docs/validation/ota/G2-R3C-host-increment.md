# G2-R3C 主机增量：DATA 前置与事务字段比对

2026-11-17；基线 `2bb2a110358aa423c5c5c4c7d7ed601c16c07e9a`，分支 `feature/imx6ull-system-ab-ota`。隔离 Git 工作区执行，未修改既有未提交 G3 文件和项目规划。未操作板端、真实 eMMC、U-Boot 环境、正式 Gateway 或 Watchdog；未推送。

**结论：G2-R3C HOST-ACCEPTED = BLOCKED，G1/G2 BOARD-TESTED = NOT RUN。** 本次交付两个未连接生产确认的只读前置库和隔离测试，不是完整的可信确认服务。

## 实际实现

- `r3c_data.[ch]`：候选 DATA 只读前置检查检查挂载身份、文件系统根/类型、rw、空间和候选 schema；通过现有 `ota_state_load` 和 `ota_oplog_read` 审查应用事务、current 和已确认 release，并在**已有**锁文件上取非阻塞共享锁，不创建或修改锁及业务状态。对历史宽松字段提取新增严格 JSON、schema 和关键字段重复/转义拒绝。
- `r3c_binding.[ch]`：检查事务 ID、包/镜像摘要、目标和 last-good 槽、boot_id、bootcount 与新鲜度是否一致；只作字段比对，调用者提供的值没有由该函数认证。
- `tests/test_g2r3c_data.py`、`tests/r3c_binding_test.c` 与测试专用 CLI：受控目录、mountinfo 样例、状态文件、锁竞争和错误注入；测试数据不作为生产可信依据。构建规则添加宿主测试和独立 ARM 测试二进制。
- 无上述库的生产调用、配置部署或确认接入。`sgw_bootctl confirm` 的原始默认拒绝路径没有改变。DATA 设计与下一步见 [架构草案](../../system-ab/09-g2-r3c-data-and-trust-design.md)。

## 结果与限制

| 工作 | 当前实际范围 | 结论 |
|---|---|---|
| 共享 DATA | 模拟 mountinfo 的正确/错误设备、只读、bind 子目录、重复挂载及损坏；真实目录拒绝作为数据块设备；应用 OTA 目录/锁/事务读取 | PARTIAL HOST-TESTED；真实独立块设备挂载成功、所有系统槽排除、持锁贯穿确认、真实写入/持久化、共享 schema/兼容均未完成 |
| 隔离文件系统镜像 | 创建 16 MiB 主机 ext4 文件并用 debugfs 读取 superblock；没有挂载 | HOST-TESTED 仅镜像创建/读取；默认主机 ext4 特性不是板端兼容证明；未验证跨槽切换 |
| 事务字段 | 16 项边界/不匹配断言，包括 ID、摘要、槽位、boot_id、bootcount、时间 | PARTIAL HOST-TESTED；无可信事务生成、包签名/镜像摘要生产来源、持久记录、抗重放及原子确认 |
| Gateway | 回归真实隔离 Supervisor/Gateway 的已有组件查询；旧 L2 在隔离上游单独成功 | PARTIAL HOST-TESTED；尚无生产无副作用完整业务转发探针 |
| N09/N10 | 没有真实生产确认路径和所有输入的可信集成 | BLOCKED / NOT RUN；不得将字段比较 PASS 说成确认 PASS |

测试环境普通用户执行 `unshare -Ur -m true` 报 `Operation not permitted`；因此没有在该隔离环境中创建 loop 挂载，也没有使用 sudo 或触及真实设备。主机 mountinfo 测试采用受控文本，不能证明真实挂载检查的成功路径。应用状态检查只持锁到返回，在与系统确认共享锁和生产集成建立之前不能防止快照后的变更。DATA 只验证当前根不同，尚未排除另一系统槽、同盘扇区重叠、克隆的 UUID、嵌套挂载和硬件身份替换。候选 schema 格式未经批准。

历史 `ota_oplog_read` 使用字符串查找而非完整 JSON 解析；新适配器对格式及重复字段做额外保守限制。当前实现仍只支持既有简单格式，遇到转义或不能证明的格式拒绝；不应理解为覆盖一切文件系统竞态或可靠的生产事务协议。

## 本轮回归

| 命令 | 实测 | 原始日志 |
|---|---|---|
| `make -C bsp/apps/boot_ab test arm-r3c-check` | 状态机 100/100、环境 31/31、健康模型 195/195、设备 21/21、网络 5/5、绑定 16/16、DATA Python 6/6；其它拒绝检查通过 | [boot-final.log](g2-r3c-host-logs/boot-final.log) |
| `make -C bsp/apps/boot_ab clean` 后 `make -C bsp/apps/boot_ab test arm-check arm-r3c-check` | 干净构建与上述测试、原有和新增 ARM 测试产物检查通过 | [clean-build.log](g2-r3c-host-logs/clean-build.log) |
| `make -C bsp/apps/boot_ab test arm-check arm-r3c-check` | 原有三个 ARM 程序和新增测试程序构建/依赖检查通过 | [boot-tests.log](g2-r3c-host-logs/boot-tests.log) |
| `UBOOT_AB_BUILD_DIR=/tmp/sgw-g1g2-audit-c2TLcv/repo/build/uboot-ab python3 -m unittest -v tests.test_g1_backup_guard tests.test_g2_uboot_ab tests.test_g2r3_health` | 25/25；使用既有隔离 U-Boot 主机构建 | [g1g2-r3.log](g2-r3c-host-logs/g1g2-r3.log) |
| `make -C bsp/apps/service_supervisor test` | 58/58、35/35 | [supervisor.log](g2-r3c-host-logs/supervisor.log) |
| `make -C bsp/apps/ota_agent test-supervisor test-supervisor-s2 HOST_SUP_BIN=/tmp/sgw-r3c-nzNYvJ/ota-sup-test HOST_S2_BIN=/tmp/sgw-r3c-nzNYvJ/ota-sup-s2-test` | 145/145、44/44 | [ota.log](g2-r3c-host-logs/ota.log) |
| `make test` | 10 个 Gateway/Server 单元测试程序 PASS | [core.log](g2-r3c-host-logs/core.log) |
| 测试专用 Gateway 构建，`python3 -m unittest -v tests.test_g2r3b_gateway_probe` | 2/2；局部组件与隔离进程，无生产业务确认 | [gateway-integration.log](g2-r3c-host-logs/gateway-integration.log) |
| `make gateway arm arm-check` | Gateway ARMv7、hard-float 构建通过，最高 glibc 符号 2.28 | [gateway-prod.log](g2-r3c-host-logs/gateway-prod.log) |
| `python3 tests/test_ota_health_e2e.py` | 隔离旧 L2 真实上游测试通过；并非无副作用生产方案 | [ota-health-e2e.log](g2-r3c-host-logs/ota-health-e2e.log) |
| `mke2fs -t ext4` 与 `debugfs -R stats` 对本轮临时镜像，统计输出经 `sed` 去除行尾空白 | 可读 superblock，未挂载 | [image-stats.log](g2-r3c-host-logs/image-stats.log) |

首次新增测试的正例因 Ubuntu 默认 umask 使 fixture 状态文件为 0664，被目录信任检查拒绝；将测试文件权限设置为 0600 后重跑通过。这是测试数据设置错误，不是生产 DATA 已验证。

ARM 测试版 DATA 工具依赖 `libjson-c.so.4` 和 `libc.so.6`，最高 glibc 符号 2.7；绑定测试依赖 libc，最高 2.4。它们是测试产物，未部署；原有 bootctl、watchdog guard 和诊断程序的 ARM 检查单独通过。
