# G1 最终验收报告（2026-11-18）

**最终结论：G1 验收通过（PASS / ACCEPTED — 满足板端独立恢复引导、双份离线全量备份校验、分区/引导区用途实证与非破坏性恢复启动验证门槛；G2–G6 仍按各自门槛受控，不得由 G1 放行直接修改 `p2` 或部署未验收的 A/B confirm）。**

- **目标平台：** 100ASK i.MX6ULL Pro（`Freescale i.MX6 ULL 14x14 EVK Board`，eMMC `S40004` `3.64 GiB`，`7,634,944` sectors）
- **分支与基准：** `feature/imx6ull-system-ab-ota`
- **原系统保护结果：** `/dev/mmcblk1p2`（现用主系统）、`mmcblk1boot0`（活跃 U-Boot）、`pre-p1` 与 `0xE0000` 环境变量全程保持只读锁保护、未修改且最终验证自动重启返回正常。

---

## 1. G1 放行门槛逐项验收矩阵（对应 [前置条件与证据 §5](../../system-ab/readiness-and-roadmap/01-prerequisites-and-evidence.md#5-写入前的恢复放行门槛)）

| # | 门槛要求 | 实测结论 | 核心证据文件 / 输出 |
|---|---|---|---|
| 1 | 固定板型/存储版本，验证恢复镜像、工具哈希及恢复覆盖范围（U-Boot/env/分区/唯一数据） | **PASS** | `PARTITION_CONFIG=0x48`（活跃 U-Boot 在 `boot0` `@0x400`，SHA-256 `ad9fb499...`）；生效 44 变量环境在 `pre-p1` `@0xE0000`（`CRC32=0x7d67365e`）；`candidate-v2/boot/SHA256SUMS` 6/6 OK；唯一数据清单已记录在 [离线审计](G1-offline-audit-and-restore-prep-20261117.md) |
| 2 | 从不依赖当前 `p2` 与 eMMC 的独立恢复入口实际进入恢复环境 | **PASS** | 拨码 `SW3=OFF, SW4=ON`（USB BootROM `15a2:0080`），`uuu` 注入隔离 `u-boot-dtb-sdp.imx`（`CONFIG_ENV_IS_NOWHERE`）+ `zImage` + `dtb` + `rescue-initramfs-v2.cpio.gz`，串口 `bootz` 进入 `candidate-v2` RAM shell，自动挂载 `devtmpfs`，eMMC 全设备锁定 `ro=1`，固定密钥 SSH `192.168.10.3:2222` 连通。日志：[SDP U-Boot](g1-20261118-sdp-uboot.log.gz)、[SDP RAM 启动](g1-20261118-sdp-ram-boot.log.gz) |
| 3 | 从停写的独立恢复环境取得一致备份（含分区表、保留区、user area、`boot0`/`boot1`；不动 RPMB） | **PASS** | 经 RAM 恢复环境锁定 eMMC `ro=1` 后通过固定密钥加密 SSH 采集 7 段（`pre-p1`、`p1`、`p2`、`p3`、`tail`、`boot0`、`boot1`），`e2fsck -fn emmc-p2.img` 退出码 `0`（`17928/89664 files`），user area 重组 `3,909,091,328 B`（`3f7e250c...`）。记录：[RAM 备份实测](G1-ram-boot-and-backup-20261117.md)、[SHA256SUMS](g1-20261117-SHA256SUMS) |
| 4 | 备份离板保存至少两份可校验副本 | **PASS** | 副本 1：Ubuntu 虚拟机 `/home/ubuntu/.sgw-g1-recovery-prep/backup-20261117/`（`g1_verify_segments.py` 7/7 OK）；副本 2：Lenovo SS380 64G U 盘 `SmartGateway-G1-20261117/`（`sha256sum -c` 7/7 PASS，[usb-sha256](g1-20261117-usb-sha256.log)） |
| 5 | 备份可恢复性与恢复后实机启动验证（不破坏唯一好 `p2`） | **PASS（等价非破坏性实机 + 全盘隔离验证）** | 主机端 `g1_restore_segments.py --mode full-disk` 7 段写回及回读 SHA-256 与原备份逐字节一致（[restore-sim](g1-20261117-restore-sim.log)）。实机端经用户授权，将离线缩小为 `990 MiB`（`1,038,090,240 B`，`e2fsck -fn` 退出码 `0`）的 `emmc-p2-for-p1.img` 写入废弃 `p1`，回读 SHA-256 `2d24f1dc...` 一致、`/sbin/e2fsck -fn /dev/mmcblk1p1` 退出码 `0`（[p1-restore](g1-20261118-p1-restore.log)）；切回 eMMC 拨码后在 U-Boot 临时设置 `mmcpart=1`、`mmcroot=/dev/mmcblk1p1 rootwait rw`（不执行 `saveenv`）实机启动验证串口、`4.9.88` 内核、`179:1` 根挂载、ADB、`eth0`（`192.168.10.3` <-> `192.168.10.2` 0% 丢包）、GPIO/I²C（`0x1e` AP3216C）、`service_supervisor`（`pid=785`）与 `smart_gateway`（`pid=1352`），随后重启自动切回未改动的原 `p2`。日志：[p1 启动](g1-20261118-p1-boot.log.gz)、[自动回切 p2](g1-20261118-return-p2.log.gz) |
| 6 | 证明 `p1`/`p3`/尾部及环境区域用途、边界与恢复方式 | **PASS** | 离线审计 [offline-audit](g1-20261117-offline-audit.log) 实证：`boot0` `@0x400` 为活跃 U-Boot，`pre-p1` `@0xE0000` 为活跃 env（`@0xE2000` 全零），`p1` 原为 `+50 MiB` 废弃 2021 ext4（现存可启动 `p2` 克隆），`p3` 500 MiB 全零，`tail` 仅 4 处旧备份超级块残留；`p2` 无 `.swu`/`sw-description`，确认走自建受控 A/B 路线 |
| 7 | 防误写与回读校验恢复工具（保护 `p2`、拒绝别名/挂载目标） | **PASS** | [g1_restore_segments.py](../../../tools/g1_restore_segments.py) 强制执行写前档案校验、挂载拒绝、`p2` 几何与设备号拦截（实机测试传入 `/dev/mmcblk1p2` 被直接拒绝）、`ForceRoGuard`/`BlkRoGuard` 只读恢复、`fsync` 与写后全量回读 SHA-256 核对 |
| 8 | 书面验收与阶段边界 | **PASS** | 本文与对应原始日志归档完成；G2/G4/G5 写入前仍须各自满足其健康确认与回滚门禁 |

---

## 2. 边界与剩余风险说明（严格实事求是）

- 因当前仅有单块开发板，为遵守“不得破坏唯一可用主系统”原则，未在板端直接覆盖现用 `mmcblk1boot0`、`pre-p1` 或 `p2`；7 段全盘（含 `boot0`/`boot1`/`pre-p1`）写回与回读一致性已由 `g1_restore_segments.py` 在主机隔离镜像上验证通过，一旦发生整盘损坏，可通过 USB OTG SDP 进入 `candidate-v2` 执行 `--mode full-disk` 恢复。
- `candidate-v2` 后续已补充 `mkimage` `uImage` 封装（`rescue-initramfs-v2.cpio.gz.u-boot`）并更新 `u-boot-dtb-sdp.imx` 的 `mfgtool_args`（通过 `uuu -dry` 预检）；实机本次 USB SDP 启动采用 `u-boot-dtb-sdp.imx` 后于串口执行 `bootz` 启动 `rescue-initramfs-v2.cpio.gz`，`g1_start_ssh.sh` 中 `/var/empty` 权限修复也已同步合入脚本源码与后续构建产物。

---

## 3. 主机端最终回归验证汇总

- `bash bsp/tools/g1_check_host_env.sh`：`PASS=12 FAIL=0`
- `make test`：10/10 PASS
- `make -C bsp/apps/service_supervisor test`：`service_ipc_test` 58/58 PASS、`service_supervisor_test` 35/35 PASS
- G1 Python 单元测试（7 套：`test_g1_backup_guard`、`test_g1_verify_segments`、`test_g1_network_transport`、`test_g1_ssh_loopback`、`test_g1_audit_offline_backup`、`test_g1_restore_segments`、`test_g1_recovery_initramfs`）：全部 PASS
