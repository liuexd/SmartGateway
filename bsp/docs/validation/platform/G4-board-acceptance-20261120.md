# G4 真机 SWUpdate `.swu` 签名包与双向 A/B 升级验收报告（2026-11-20）

> **状态：** `BOARD-ACCEPTED`（物理 `100ASK_IMX6ULL` 开发板已完成主机全量回归、负向门禁实测、`p1 -> p2` 真实 `.swu` 试启动确认，以及 `p2 -> p1` 反向 `.swu` 连续升级与防重放验收；出厂 `p2` 与升级前快照已在刷写前离板归档保存）。

---

## 1. 本轮 G4 真机闭环修复与工程改造

1. **策略驱动 `protect_slot2`（默认 Fail-Closed 保护，不再硬编码阻塞）：**
   - [sgw_bootctl.c](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/apps/boot_ab/sgw_bootctl.c) 与 [sgw-swu-apply.sh](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/ota/sgw-swu-apply.sh) 移除编译期和脚本层对 `target == 2` 的硬编码拦截，统一以 root 属主 `/etc/smartgateway/r3c-policy.conf`（`0600`）作为唯一授权来源；`r3c_policy_init_default()` 仍保持缺省 `protect_slot2=1` Fail-Closed。
2. **修复连续多轮升级历史已消费事务拦截 Bug（实机首轮捕获）：**
   - 原 `r3c_bundle.c` 中的 `no_existing_record()` 会将 `/opt/smartgateway/data/.sgw-tx/` 中前一轮已结算的 `CONFIRMED`（`consumed=1`）事务一并拒绝，阻断后续连续升级。
   - [r3c_bundle.c](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/apps/boot_ab/r3c_bundle.c) 将其升级为 `no_active_record(txdir, owner)` + `is_settled_txn_file()`：继续 Fail-Closed 拒绝任何未完结事务（`VERIFIED`/`WRITTEN`/`PENDING`/`CONFIRMING`、`consumed=0`）、孤立 `.manifest` 或杂项/软链文件，仅放行格式完整且已确认消费的历史记录。
3. **支持独立可自确认 ARM RootFS 构建选项：**
   - [post_build_g3.sh](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/rootfs/post_build_g3.sh) 新增可选构建环境变量 `SGW_OTA_PUBKEY_FILE`、`SGW_R3C_POLICY_FILE`、`SGW_ETH0_STATIC_IP`，在不影响默认 G3 构建行为的前提下支持生成预置 `ota-public.pem`、`r3c-policy.conf` 与静态 `eth0` 的真机候选镜像。

---

## 2. 主机端编译与全量回归实测结果

所有代码修改完成后重新执行主机构建、ARM 交叉编译、镜像重建与全量 Python 回归：

| 验证项目 | 执行命令与摘要 | 日志 |
| --- | --- | --- |
| `boot_ab` x86-64 / 隔离构建 / ARM 与 C/R3C 单测 | `make -C bsp/apps/boot_ab clean all sgw_bootctl_isolated arm-check test`：`ab_state_test` `100/100`、`fw_env_backend_test` `31/31`、`ab_health_test` `FAIL=0`、`system_health_test` `195/195`、`health_devices_test` `21/21`、`health_network_test` `5/5`、`r3c_binding_test` `16/16`、`r3c_data_exclusion_test` `PASS`、`test_g2r3c_data` `7/7 OK`、`test_g2r3c_signed_bundle` `6/6 OK` | [01-boot-ab-build-and-c-tests.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/01-boot-ab-build-and-c-tests.log) |
| G4 ARM RootFS 重建与成员审计 | `SGW_OTA_PUBKEY_FILE=... SGW_R3C_POLICY_FILE=... SGW_ETH0_STATIC_IP=192.168.10.3 ./bsp/rootfs/build_rootfs_g3.sh`（19s）：`rootfs.ext4` (`134217728` B, SHA-256 `98a4a13ba1b8215e85b6e1aad277e445eaed1f605522983c4f7607716765eee0`)、`sgw_bootctl` (`4475b9b5ae62d8dd44ad1b70f54915e8f4f77fdafbae7ab184ae9d0789713c3e`)、`sgw_watchdog_guard` (`6d8b2f92eae71984392b43fa068cb88c6afe5191ebf0e060c5017139141f0220`)、`swupdate` (`d4280cf11e5b785a5d038878bf43a8e5f9802f1a72de38ef10abb397b5f5ca78`) | [01b-arm-rootfs-build.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/01b-arm-rootfs-build.log)、[01c-rootfs-tar-audit.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/01c-rootfs-tar-audit.log) |
| G4 SWUpdate 隔离专项（含连续升级） | `SGW_TEST_FUSE2FS=/usr/bin/fuse2fs python3 -m unittest -v tests/test_g4_swu_isolated.py`：**10/10 OK**（`9.851s`） | [01d-test-g4-swu-isolated.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/01d-test-g4-swu-isolated.log) |
| G3 / Review4 专项回归 | `SGW_TEST_FUSE2FS=/usr/bin/fuse2fs python3 -m unittest -v tests/test_g3_kernel_rootfs.py tests/test_review4_g3_bundle.py tests/test_review4_topology.py`：**6/6 OK**（`1.016s`） | [01e-test-g3-and-review4.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/01e-test-g3-and-review4.log) |
| 全量 `test_g*.py` 主机回归 | `SGW_TEST_FUSE2FS=/usr/bin/fuse2fs python3 -m unittest discover -s tests -p 'test_g*.py' -v`：**102/102 OK**（`85.760s`） | [01f-test-g-suite.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/01f-test-g-suite.log) |
| 候选 `.swu` 打包与主机 `swupdate -c` 验签 | `SmartGateway-g4-v2.1.0-slot2.swu`（SHA-256 `402dcaef3a8ee3ede6af46b756572a1b2dda7f046d9aa6b55758f04faec1321c`）与 `SmartGateway-g4-v2.1.1-slot1.swu`（SHA-256 `9af489186462b3145dbd7fd8ca257039ef00a13071d4d0b9c379df03212ce643`）通过；`SmartGateway-g4-untrusted-slot2.swu`（SHA-256 `d0abbf8da198a4285f985592e33fe4f6ccf3e2ee425f3be9271ce7674a6b0a79`）被 `swupdate -c` 拒绝（`rc=1`） | [01g-swu-host-check.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/01g-swu-host-check.log) |

---

## 3. 开发板真机验收实测结果（`100ASK_IMX6ULL`）

### 3.1 板端负向安全门禁实测（`p1` 运行态 + `protect_slot2=1`）

| 测试项 | 触发包 | 实测结果 | 日志 |
| --- | --- | --- | --- |
| **N1 非受信任私钥签名** | `SmartGateway-g4-untrusted-slot2.swu` | `EVP_DigestVerifyFinal failed`，`swupdate` 拒绝（`N1_RC=1`），`p1` 状态与 env 零变更 | [02-board-negative-gates.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/02-board-negative-gates.log) |
| **N2 错误目标槽（当前运行槽 1）** | `SmartGateway-g4-v2.1.1-slot1.swu` | RSA 验签后被拒绝：`APPLY REJECTED: target slot conflicts with running or last_good slot`（`N2_RC=1`） | [02-board-negative-gates.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/02-board-negative-gates.log) |
| **N3 策略 `protect_slot2=1`** | `SmartGateway-g4-v2.1.0-slot2.swu` | RSA 验签后被拒绝：`APPLY REJECTED: target slot 2 is write-protected factory baseline`（`N3_RC=1`） | [02-board-negative-gates.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/02-board-negative-gates.log) |

### 3.2 双向真实 `.swu` 升级与 `sgw_watchdog_guard` 试启动确认闭环

1. **升级 #1（`p1 -> p2`，策略 `protect_slot2=0`）：**
   - `/usr/bin/sgw-swu-install .../SmartGateway-g4-v2.1.0-slot2.swu` 完成 RSA 验签、`debugfs` 校验、写入 `/dev/mmcblk1p2`（`134217728` B）与读回提交：`STAGED tx=0a039dcc6ca9973223a4fd20491bed63e248dc313ff95bf11d40a1ee6e8400e0 target=2 last_good=1`（`APPLY_RC=0`），`sgw_ab_state=trial`、`upgrade_available=1`。
   - 重启后成功从 `/dev/mmcblk1p2` 启动，通过 `S15sgw_ab_guard -> sgw_watchdog_guard` R3C 确认并干净停狗：
     - `sgw_bootctl status`：`sgw_active_slot=2`、`sgw_last_good_slot=2`、`sgw_pending_slot=none`、`sgw_ab_state=confirmed`、`upgrade_available=0`、`bootcount=0`
     - 事务记录：`state=CONFIRMED`、`consumed=1`、`boot_id=b3fe565f-fe21-446f-aa35-67347aec4013`、`bootcount=1`
     - 清单实时核对：**`MANIFEST_SUMMARY ok=15 bad=0`**
     - 稳态持续运行至 `90.08s` 且 `boot_id` 不变（零 WDOG 复位）
   - 原始日志：[03-board-upgrade-p1-to-p2.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/03-board-upgrade-p1-to-p2.log)、[04-board-verify-p2-confirmed.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/04-board-verify-p2-confirmed.log)。

2. **升级 #2（`p2 -> p1`，连续反向升级）：**
   - 在已确认运行的 `p2` 槽位直接执行 `/usr/bin/sgw-swu-install .../SmartGateway-g4-v2.1.1-slot1.swu`：完整保留前置历史事务 `txn-acc7c48c...` 与 `txn-0a039dcc...`，提交新事务 `STAGED tx=f9545202d13077ab8f6c7a3ebf164fd9548e52e80c52e61b5de9b1e14d35ad76 target=1 last_good=2`（`APPLY_RC=0`）。
   - 重启后成功从 `/dev/mmcblk1p1` 启动并完成 R3C 确认与稳态防重放校验：
     - `sgw_bootctl status`：`sgw_active_slot=1`、`sgw_last_good_slot=1`、`sgw_pending_slot=none`、`sgw_ab_state=confirmed`、`upgrade_available=0`、`bootcount=0`
     - 事务记录：`state=CONFIRMED`、`consumed=1`、`boot_id=fd31ffc4-5061-443d-93d2-99ae87cd3ff6`、`bootcount=1`
     - 清单实时核对：**`MANIFEST_SUMMARY ok=15 bad=0`**
     - 稳态防重放：`sgw_bootctl confirm` 拒绝并输出 `CONFIRM REJECTED: no active unconsumed transaction record`（`REPLAY_CONFIRM_RC=1`）
     - 稳态持续运行至 `130.64s` 且 `boot_id` 不变（零 WDOG 复位）
   - 原始日志：[05-board-upgrade-p2-to-p1.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/05-board-upgrade-p2-to-p1.log)、[06-board-verify-p1-confirmed.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/06-board-verify-p1-confirmed.log)。

---

## 4. 离线恢复与边界声明

- **离线出厂 `p2` 恢复基线仍可随时回写：** 2026-11-17 原始出厂恢复包（`/home/ubuntu/.sgw-g1-recovery-prep/p2-factory-recovery-20261117/` 及 U 盘副本）与 2026-11-19 升级前全量快照（`/home/ubuntu/.sgw-g1-recovery-prep/snapshot-before-g4-20261119/`）均保持完整可用，不受本次 `p2` 双向轮转测试影响。
- **策略边界声明：** 真机验证使用 `gateway_flow_mode=controlled_local`、`allow_external_degraded=1` 与 `require_sensor_heartbeat=0`（且 `gpio`、`ap3216_direct`、`local_network` 真实健康），不含 `strict_forward` 北向实网转发与 `require_sensor_heartbeat=1` 的外网现场认证。
