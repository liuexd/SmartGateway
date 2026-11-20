# G4 真机 SWUpdate `.swu` 签名包与完整 A/B 升级验收报告（2026-11-20）

> **状态：** `BOARD-ACCEPTED`（按项目路线图第 10 节完整条件收口，策略边界为 `gateway_flow_mode=controlled_local` / `allow_external_degraded=1` / `require_sensor_heartbeat=0`）。物理 `100ASK_IMX6ULL` 开发板已完成主机全量与故障注入回归、板端负向安全门禁、`p1 -> p2 -> p1` 双向真实 `.swu` 试启动确认、**失败候选 `WDOG1` x3 -> `bootlimit=3` 自动回退 -> 自动 `ROLLED_BACK` 结算 -> 旧槽业务与共享 `p3` DATA 恢复 -> 回退后直接安装下一合法 `.swu` 确认升级至 `p2`** 全链路真机闭环，并补齐密钥信任根/轮换/吊销策略与离线恢复介质校验。

---

## 1. 本轮 G4 真机闭环修复与工程改造

1. **策略驱动 `protect_slot2`（默认 Fail-Closed 保护，不再硬编码阻塞）：**
   - [sgw_bootctl.c](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/apps/boot_ab/sgw_bootctl.c) 与 [sgw-swu-apply.sh](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/ota/sgw-swu-apply.sh) 移除编译期和脚本层对 `target == 2` 的硬编码拦截，统一以 root 属主 `/etc/smartgateway/r3c-policy.conf`（`0600`）作为唯一授权来源；`r3c_policy_init_default()` 仍保持缺省 `protect_slot2=1` Fail-Closed。
2. **修复连续多轮升级历史已消费事务拦截 Bug（实机首轮捕获）：**
   - 原 `r3c_bundle.c` 中的 `no_existing_record()` 会将 `/opt/smartgateway/data/.sgw-tx/` 中前一轮已结算的 `CONFIRMED`（`consumed=1`）事务一并拒绝，阻断后续连续升级。
   - [r3c_bundle.c](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/apps/boot_ab/r3c_bundle.c) 将其升级为 `no_active_record(txdir, owner)` + `is_settled_txn_file()`：继续 Fail-Closed 拒绝任何未完结事务（`PENDING`/`CONFIRMING`、`consumed=0`）、孤立 `.manifest` 或杂项/软链文件，放行格式完整且已结算（`CONFIRMED` 或 `ROLLED_BACK` 且 `consumed=1`）的历史记录。
3. **补齐 U-Boot 回退后自动事务结算与 `bootcount=0` 边界修复（源码审计 + 实机回退复现修复）：**
   - 问题关闭：U-Boot `bootlimit=3` `altbootcmd` 将损坏候选自动切回 `last_good` 后，`.sgw-tx/` 下残余 `PENDING`/`CONFIRMING` 记录若无自动结算路径，会在下一次升级时拦截合法包；此外在实机回归中捕获到 `r3c_txn_reconcile_rollback()` 以当前启动槽 `boot_id` 和回退后 `bootcount=0` 落盘时，原 `r3c_txn.c` 会写出 `bootcount=0` 且 `read_record_file()` 无条件拒绝 `bootcount == 0`（`invalid transaction bootcount`）。
   - 修复实现：在 [r3c_txn.h](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/apps/boot_ab/r3c_txn.h)、[r3c_txn.c](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/apps/boot_ab/r3c_txn.c)、[r3c_bundle.c](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/apps/boot_ab/r3c_bundle.c)、[r3c_apply.c](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/apps/boot_ab/r3c_apply.c) 与 [sgw_watchdog_guard.c](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/apps/boot_ab/sgw_watchdog_guard.c) 中新增 `R3C_TXN_ROLLED_BACK`（`state=ROLLED_BACK`、`consumed=1`）与 `r3c_txn_reconcile_rollback()`；当且仅当 U-Boot env 已验证为 `sgw_ab_state=rolled_back && upgrade_available=0 && sgw_pending_slot=none` 且事务槽位与当前运行恢复槽一致时，由 `sgw_watchdog_guard` 稳态入口及 `r3c_apply` 入口自动结算为 `ROLLED_BACK / consumed=1`；同时修正 `serialize_and_commit()`（`bootcount == 0` 时不写出 `bootcount=0`）与 `read_record_file()`（仅允许 `state=ROLLED_BACK` 携带历史 `bootcount=0`，`PENDING`/`CONFIRMING`/`CONFIRMED` 仍严格拒绝 `0`）。
4. **支持独立可自确认 ARM RootFS 构建与公钥轮换/吊销规范：**
   - [post_build_g3.sh](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/rootfs/post_build_g3.sh) 支持 `SGW_OTA_PUBKEY_FILE`、`SGW_R3C_POLICY_FILE`、`SGW_ETH0_STATIC_IP` 环境变量用于候选镜像构建；[10-g4-swupdate-and-ab-writer-design.md](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/system-ab/10-g4-swupdate-and-ab-writer-design.md) 新增第 4 节“公钥信任根、密钥轮换与吊销策略”。

---

## 2. 主机端编译、故障注入与全量回归实测结果

在完成上述 `bootcount=0` 修复并扩展隔离单测后，重新执行主机构建、ARM 交叉编译、RootFS 重建与全量 Python 回归：

| 验证项目 | 执行命令与摘要 | 日志 |
| --- | --- | --- |
| `boot_ab` x86-64 / 隔离构建 / ARM 与 C/R3C 单测 | `make -C bsp/apps/boot_ab clean all sgw_bootctl_isolated arm-check test`：`ab_state_test` `PASS=100 FAIL=0`、`fw_env_backend_test` `PASS=31 FAIL=0`、`ab_health_test` `FAIL=0`、`system_health_test` `checks=195 failures=0`、`health_devices_test` `checks=21 failures=0`、`health_network_test` `checks=5 failures=0`、`r3c_binding_test` `checks=16 failures=0`、`r3c_data_exclusion_test` `PASS`、`test_g2r3c_data` `7/7 OK`、`test_g2r3c_signed_bundle` `6/6 OK` | [01-boot-ab-build-and-c-tests.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/01-boot-ab-build-and-c-tests.log) |
| G4 ARM RootFS 重建与成员审计 | `SGW_OTA_PUBKEY_FILE=... SGW_R3C_POLICY_FILE=... SGW_ETH0_STATIC_IP=192.168.10.3 ./bsp/rootfs/build_rootfs_g3.sh`（19s）：`rootfs.ext4` (`134217728` B, SHA-256 `72d5e933ec4603943f27e15bb8c21567a6922691dd38d74d38fa49c76dfc79ca`)、`sgw_bootctl` (`360952` B, `5afa6dd4f455acb88e26087273f26bd32853ee6a6577bcaa3ec314cc04afff93`)、`sgw_watchdog_guard` (`357224` B, `f429ba993e7b2e26583f48e58f2ea71545bbfceed6190d23272361a7f5b9fd26`)、`swupdate` (`96592` B) | [01b-arm-rootfs-build.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/01b-arm-rootfs-build.log)、[01c-rootfs-tar-audit.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/01c-rootfs-tar-audit.log) |
| `ROLLED_BACK` 与已消费事务规则单测 | `python3 -m unittest -v tests/test_g2r3c_signed_bundle.py`：**6/6 OK**（验证 `state=ROLLED_BACK + consumed=1` 放行而 `consumed=0` 拒绝） | [01h-test-g2r3c-signed-bundle.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/01h-test-g2r3c-signed-bundle.log) |
| G4 SWUpdate 隔离专项（含回读失败与回退重放恢复） | `SGW_TEST_FUSE2FS=/usr/bin/fuse2fs python3 -m unittest -v tests/test_g4_swu_isolated.py`：**12/12 OK**（`11.389s`，零跳过），新增覆盖 `test_readback_digest_mismatch_never_stages_pending`（`LD_PRELOAD` 劫持 `pread`/`pread64` 注入槽位回读 4KiB 篡改，断言 `slot readback digest mismatch`、`pending=none`、`last_good=1` 且零 `.sgw-tx` 提交）与 `test_rollback_reconciles_stale_pending_and_accepts_next_valid_swu`（含 `boot_id` + 遗留 `bootcount=0` 回退记录兼容） | [01d-test-g4-swu-isolated.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/01d-test-g4-swu-isolated.log) |
| G3 / Review4 专项回归 | `SGW_TEST_FUSE2FS=/usr/bin/fuse2fs python3 -m unittest -v tests/test_g3_kernel_rootfs.py tests/test_review4_g3_bundle.py tests/test_review4_topology.py`：**6/6 OK**（`0.847s`，零跳过） | [01e-test-g3-and-review4.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/01e-test-g3-and-review4.log) |
| 全量 `test_g*.py` 主机回归 | `SGW_TEST_FUSE2FS=/usr/bin/fuse2fs python3 -m unittest discover -s tests -p 'test_g*.py' -v`：**104/104 OK**（`88.773s`，零跳过） | [01f-test-g-suite.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/01f-test-g-suite.log) |
| 候选 `.swu` 打包与主机 `swupdate -c` 验签 | 双向包 `SmartGateway-g4-v2.1.0-slot2.swu` (`402dcaef3a8ee3ede6af46b756572a1b2dda7f046d9aa6b55758f04faec1321c`)、反向包 `SmartGateway-g4-v2.1.1-slot1.swu` (`9af489186462b3145dbd7fd8ca257039ef00a13071d4d0b9c379df03212ce643`)、坏健康候选 `SmartGateway-g4-v2.2.0-bad-candidate-slot2.swu` (`c2c0d0a5c80354ee480e55c71aedd9ae8dc5439f62e7afd39be215646292ebc9`) 与修复后合法候选 `SmartGateway-g4-v2.2.1-good-slot2.swu` (`806d10c9e58f4370af5f136b7764f673fa65525bb01600ea8f33a8b898dbb01f`) 均通过 `swupdate -v -k ... -H imx6ull-100ask:revA -c`；非受信任包 `SmartGateway-g4-untrusted-slot2.swu` (`d0abbf8da198a4285f985592e33fe4f6ccf3e2ee425f3be9271ce7674a6b0a79`) 被拒（`rc=1`） | [01g-swu-host-check.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/01g-swu-host-check.log) |

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

### 3.3 候选健康失败自动回退、旧槽业务与 DATA 恢复及回退后下一合法包受理闭环

1. **阶段 A — 签名坏健康候选 `.swu` 写入 `p2` 与 `WDOG1` x3 -> `bootlimit=3` 自动回退：**
   - 将 `/opt/smartgateway/releases/0.6.0/smart_gateway` 替换为 `exit 1` 的坏健康根文件系统（`rootfs-bad-health.ext4`，SHA-256 `5b5d95b0ce3f2efc21b3c408f43915c199698550275b4dc0350782f7debfadc3`，`e2fsck -fn` 洁净）签发为 `SmartGateway-g4-v2.2.0-bad-candidate-slot2.swu`（`c2c0d0a5c80354ee480e55c71aedd9ae8dc5439f62e7afd39be215646292ebc9`），通过 `/usr/bin/sgw-swu-install` 写入 `/dev/mmcblk1p2` 并提交 `STAGED tx=e43760c3ef47653e838c31bc107260413b8fff6dafb543149cf56fa93da54ac3 target=2 last_good=1`（`APPLY_RC=0`，`sgw_rollback_count=3`）。
   - 板端进入 `p2` 试启动后因 `smart_gateway` 异常退出，经连续 3 次健康超时（第 3 次观察到 `bootcount=3` / `sgw_last_error=health_timeout`）触发 `WDOG1` 复位，达到 `bootlimit=3` 后由 U-Boot `altbootcmd` 自动切回 `p1`。
2. **阶段 B — 自动 `ROLLED_BACK` 结算与旧槽业务 / 共享 `p3` DATA 恢复：**
   - 回到 `p1` 后（`/proc/cmdline` 为 `root=/dev/mmcblk1p1`），`sgw_bootctl status` 实测为 `sgw_active_slot=1`、`sgw_last_good_slot=1`、`sgw_pending_slot=none`、`sgw_ab_state=rolled_back`、`sgw_last_error=bootlimit_exceeded`、`sgw_rollback_count=4`、`bootcount=0`、`upgrade_available=0`。
   - 无需任何人工干预，`txn-e43760c3ef47653e838c31bc107260413b8fff6dafb543149cf56fa93da54ac3` 已由 `sgw_watchdog_guard` 自动结算为 **`state=ROLLED_BACK`、`consumed=1`**。
   - 旧槽业务与共享数据完整恢复：`/dev/mmcblk1p3` 正常挂载于 `/opt/smartgateway/data`（`.sgw-data-schema` 为 `schema=1`，历史 `.sgw-tx/` 全数保留）；`sgw_ap3216 0-001e`（`old=0x03 current=0x03`）与 `sgw-gpio-event` 就绪，`service_supervisor`（PID `258`）与 `smart_gateway`（PID `275`，监听 `0.0.0.0:7000`）稳定运行；`sgw_system_health --once` 实测 `root_identity=HEALTHY`、`supervisor=HEALTHY`、`gateway_components=HEALTHY`、`gpio=HEALTHY`、`ap3216_direct=HEALTHY`、`local_network=HEALTHY`；系统稳定运行至 `73.07s` 且 `boot_id=6b31ed54-2faf-42fe-a98c-54011f11a81a` 保持不变。
   - 原始日志：[07-board-failed-candidate-rollback-recovery.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/07-board-failed-candidate-rollback-recovery.log)。
3. **阶段 C/D — 发现并修复 `bootcount=0` 边界问题后，回退态直接受理下一合法 `.swu` 并确认 `p2`：**
   - 首次在回退态 `p1` 直接安装下一包时，真实捕获到 `txn-e43760c3...` 内残留 `bootcount=0` 导致 `APPLY REJECTED: invalid transaction bootcount`；完成第 1 节 `r3c_txn.c` 兼容修复并重编部署 `sgw_bootctl` / `sgw_watchdog_guard` 后，**未清理或修改 `/opt/smartgateway/data/.sgw-tx/` 任何历史文件**，直接安装 `SmartGateway-g4-v2.2.1-good-slot2.swu`（`806d10c9e58f4370af5f136b7764f673fa65525bb01600ea8f33a8b898dbb01f`）一次通过：
     - `STAGED tx=235067cac350a9077c4790f636da730fda3303c4507df66fe6ee4ccb531208a7 target=2 last_good=1`（`APPLY_RC=0`），`sgw_ab_state=trial`、`sgw_pending_slot=2`、`upgrade_available=1`、`sgw_rollback_count=4`。
   - 重启后从 `/dev/mmcblk1p2` 启动并完成 R3C 健康确认：
     - `sgw_bootctl status`：`sgw_active_slot=2`、`sgw_last_good_slot=2`、`sgw_pending_slot=none`、`sgw_ab_state=confirmed`、`bootcount=0`、`upgrade_available=0`、`sgw_rollback_count=4`、`sgw_last_error=none`
     - `txn-235067ca...` 已结算为 `state=CONFIRMED`、`consumed=1`、`boot_id=ac4237d6-b1de-408b-93de-914985ca11ca`、`bootcount=1`；历史 `txn-e43760c3...` 仍完整保留为 `state=ROLLED_BACK`、`consumed=1`
     - 清单实时核对：**`MANIFEST_SUMMARY ok=15 bad=0`**
     - 业务与数据健康：`/dev/mmcblk1p3`（`schema=1`）、`service_supervisor`（PID `410`）、`smart_gateway`（PID `463`），`sgw_system_health --once` 各项本地健康均 `HEALTHY`，`sgw_bootctl confirm` 返回 `REPLAY_CONFIRM_RC=1`
     - 稳态持续运行至 `76.12s` 且 `boot_id=ac4237d6-b1de-408b-93de-914985ca11ca` 保持不变
   - 原始日志：[08-board-post-rollback-next-upgrade.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/08-board-post-rollback-next-upgrade.log)、[09-board-post-rollback-p2-confirmed.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261120-board-logs/09-board-post-rollback-p2-confirmed.log)。

---

## 4. 离线恢复介质核验、密钥生命周期与边界声明

- **离线出厂 `p2` 恢复包与 U 盘复核：**
  - VM 副本（`/home/ubuntu/.sgw-g1-recovery-prep/p2-factory-recovery-20261117/`）与 2026-11-19 升级前全量快照（`/home/ubuntu/.sgw-g1-recovery-prep/snapshot-before-g4-20261119/`）均校验完整；
  - Windows 宿主机只读核验物理 U 盘 `E:\p2-factory-recovery-20261117\SHA256SUMS` 全数通过（**`USB_SHA256_SUMMARY ok=15 fail=0`**）；同时测得 `Get-Volume -DriveLetter E`（exFAT，`SS380 64G`）存在脏卷标志（`HealthStatus: Warning`、`OperationalStatus: Full Repair Needed`），解释了此前 VM 下 `/mnt/usb` 的 `I/O error`，建议后续在 Windows 安全备份后择机修复该 U 盘文件系统并重新只读挂载校验。
- **公钥信任根、密钥轮换与吊销策略：**
  - 完整机制见 [10-g4-swupdate-and-ab-writer-design.md](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/system-ab/10-g4-swupdate-and-ab-writer-design.md) 第 4 节（`/etc/smartgateway/ota-public.pem` 单一信任根、`ota-priv.pem` 离线隔离、“旧钥签新公钥 -> 新钥覆盖旧槽”双槽两步计划轮换、泄露紧急覆盖与 BootROM 离线救砖边界）。
- **策略边界声明：**
  - 真机验证使用 `gateway_flow_mode=controlled_local`、`allow_external_degraded=1` 与 `require_sensor_heartbeat=0`（实测 `gpio`、`ap3216_direct`、`local_network` 真实健康），不含 `strict_forward` 北向实网转发与 `require_sensor_heartbeat=1` 的外网现场认证。
