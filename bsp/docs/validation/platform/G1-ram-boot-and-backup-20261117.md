# G1：RAM 恢复启动和离线备份实测记录

**结论：G1 仍为 BLOCKED。** 已证明在原厂 U-Boot 可用的前提下从网络加载临时 RAM 系统，并在该环境中完成 eMMC 离线分段备份、校验及主机镜像重组；尚未证明 eMMC 引导损坏时的独立恢复入口，也未执行真实设备写回及恢复后启动验证。此记录不是 G2 回滚验收。

## 已执行与证据

- 启动前根为 `/dev/mmcblk1p2` rw。首次候选 initramfs 在内核解包时失败并 panic；未写入 eMMC。用户复位后重新确认原 p2 启动。记录：[首次串口日志](g1-20261117-first-boot.log.gz)。
- 精简候选通过 TFTP 接收长度和内存 CRC32 校验，执行 `bootz` 后进入 RAM shell；`/proc/self/mountinfo` 显示 `/` 是 `rootfs`，没有 eMMC 读写挂载。手动挂载 `devtmpfs` 后，核验 eMMC 几何，并将整盘、p1/p2/p3 和 boot0/boot1 临时设置为只读；这些状态随重启失效。SSH host key 从串口核对后用于连接。记录：[RAM 启动与校验串口日志](g1-20261117-ram-boot.log.gz)。
- 通过加密 SSH 将预留区、p1、p2、p3、尾部及 boot0/boot1 接收至虚拟机，逐段源端 SHA-256、接收端落盘回读校验；[传输记录](g1-20261117-backup.log)、[校验清单](g1-20261117-SHA256SUMS)。
- 独立 `sha256sum -c` 七项全部通过，见[记录](g1-20261117-sha256.log)。p2 备份文件只读 `e2fsck -fn` 退出码 0，见[记录](g1-20261117-p2-e2fsck.log)。
- 在虚拟机中将 user area 五段重组成普通镜像文件，长度 3,909,091,328 字节；回读 SHA-256 为 `3f7e250c3f6cd842974f5e17a6a4617196c3141f410dca53a6ffacfa222ee7f0`，分区表与基线一致。boot0/boot1 仍为独立文件。见[重组记录](g1-20261117-reconstruction.log)。
- 恢复环境中通过 `reboot` 返回原系统，ADB 再次识别，`root=/dev/mmcblk1p2 rootwait rw`、原 `bootcmd` 保持。没有对 eMMC 或 U-Boot 环境进行持久写入；正常原系统重启后的自身写入不属于离线备份。

## 文件位置及限制

- 虚拟机备份：`/home/ubuntu/.sgw-g1-recovery-prep/backup-20261117/`（七个分段和 `SHA256SUMS`）。
- 虚拟机离线重组：`/home/ubuntu/.sgw-g1-recovery-prep/reconstruction-20261117.img`。备份可能包含用户数据，应限制权限并妥善保管；镜像不包含 eMMC boot0/boot1。
- 恢复启动依赖当时板上已有 U-Boot 以及电脑提供的内核、DTB 和 initramfs；这不证明 eMMC 整盘或 U-Boot 损坏后的救援方式。未进行断电、刷写、`saveenv` 或破坏性恢复。
- 首次大镜像失败可能与资源有关，但现有日志不能证明唯一原因。精简镜像启动后需要手动挂载 `devtmpfs`，SSH 启动脚本需临时修正为绝对路径；因此该候选尚不是经过工程化验收的恢复发行版。
- Lenovo SS380 64G exFAT U 盘已在 Ubuntu 识别为 `/dev/sdb1`，向新目录 `SmartGateway-G1-20261117` 复制七个分段并回读校验；独立 `sha256sum -c` 七项全部通过，见 [复制记录](g1-20261117-usb-copy.log) 与 [独立校验](g1-20261117-usb-sha256.log)。用户已卸载，`findmnt` 和 `lsblk` 确认不再挂载。没有格式化或覆盖 U 盘现有文件。实机恢复写回和恢复后启动验证尚未进行。
