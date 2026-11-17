# G1 板端独立恢复与离线备份实操方案（待授权执行）

> **目的：** 在不修改现有 eMMC 分区、U-Boot 或 `p2` 业务的前提下，补齐 G1 最核心的 P0 真机证据（外部恢复介质独立启动 + `/dev/root` 拒绝误写 + 全量镜像离线备份与 `SHA256SUMS` + 可恢复验证）。

## 1. 事前检查（严格只读）

1. 准备一张容量 `>= 8 GiB` 的 MicroSD 卡（`FAT32`/`ext4` 双分区或包含原厂可启动恢复系统的 SD 镜像）与 USB-TTL 串口线连接调试串口 `ttymxc0`（`115200 8N1`）。
2. 将开发板拨码开关切至 **SD 卡启动模式**（不挂载/不修改板载 eMMC）。

## 2. 串口阶段记录（外部启动介质证据）

1. 上电捕获完整串口启动日志至外部介质可启动的 Linux shell：确认内核与根文件系统来自 SD 卡（`mmcblk0p*`），而非板载 eMMC（`mmcblk1`）。
2. 运行只读布局审计命令并留存原始串口记录：
   - `lsblk -b -o NAME,MAJ:MIN,SIZE,FSTYPE,MOUNTPOINT`
   - `fdisk -l /dev/mmcblk1`
   - `dd if=/dev/mmcblk1 bs=512 skip=1792 count=32 2>/dev/null | sha256sum`（仅只读采样子区域指纹，核查 `0xE0000` 区域现状）
   - 核查 `p1` 与 `p3` 实际文件系统/内容（只读挂载或 `file -s` / `fsck -n`）并确认用途。

## 3. 防自毁拒绝误写验证 + 离线全量备份

1. 将仓库脚本 [bsp/tools/g1_emmc_backup_verify.sh](../../tools/g1_emmc_backup_verify.sh) 复制到外部恢复系统的 `/tmp/`：
   - **必做拒绝测试（在开发板 eMMC 正常启动态下，仅传当前根路径验证不会写盘）：** 目标设为 `/` 或当前根挂载点下目录，验证脚本因 `/dev/root` / 同设备防护立即拒绝退出且无目录创建。
2. 在 **SD 卡恢复系统下**（确保 `/dev/mmcblk1*` 全部只读挂载或未挂载），将备份目标指向 SD 卡或挂载的外部 U 盘/NFS 目录，执行只读离线全量备份：
   - 整盘镜像 `mmcblk1.raw` + 分区表 `sfdisk -d /dev/mmcblk1` + 关键分区镜像 + `env` 原始扇区备份；
   - 生成并校验 `sha256sum -c SHA256SUMS`。

## 4. 恢复路径验证闭环

- 依据原厂资料与本轮采集的 `SHA256SUMS`，确认从 SD 卡或 NXP `uuu`/MFGTool 完整写回 eMMC 的精确步骤并保留操作核对表；只有上述证据归档签收后，才可进入 G2 真机试启动与 G4 写入授权评审。
