# G4 离线设计：SWUpdate `.swu` 签名包、A/B 槽位写入与回读接口规范

> **状态：** `DESIGN / OFFLINE-PREP`（面向 G4 离线隔离镜像实现；严禁在 G1 板端恢复门禁完成前写入真实 `/dev/mmcblk1` 分区）。

## 1. 上下游对接总览

G4 负责把 G3 产出的裁剪产物（`zImage`、`100ask_imx6ull-14x14.dtb`、`rootfs.ext4`）安全封装并写入经批准的**非活动系统槽**，并与 G2-R3C 已完成的可信确认链对接：

```text
[G3 产物: zImage / dtb / rootfs.ext4]
        │
        ▼ (RSA-2048 私钥离线签发 sw-description / manifest)
[SWUpdate .swu 签名升级包]
        │
        ▼ (G4 写入协调器: 持 .ota.lock 排他 + .sgw-transaction.lock 排他)
  1) 验签 + 校验 board=imx6ull-100ask + rootfs.manifest (SGW-SYSTEM-2 不可变系统文件清单) + SHA-256 -> r3c_bundle_record() [VERIFIED]
  2) 调用自校验写槽接口 r3c_txn_write_and_verify_slot(txdir, id, owner, imgfd, fw_cfg, hcfg, opts) [WRITTEN]：
     函数内部独立校验 running_slot / last_good / mountinfo 未挂载 / !S_ISLNK && S_ISBLK / BLKGETSIZE64 容量，然后写槽 + fsync + 读回 SHA-256
  4) 原子写 U-Boot env + 读回核验 -> r3c_txn_stage_pending() [PENDING]
        │
        ▼ (重启进入候选槽 + WDOG1 守护)
[G2-R3C 协调器: r3c_coordinator_confirm()] -> [CONFIRMING -> fw_env CONFIRMED -> consumed=1]
```

## 2. 需在 G4 阶段审计与固化的接口契约

1. **签名包与信任链：**
   - 统一 `.swu`（`cpio` newc 格式，含 `sw-description` + `sw-description.sig` + `zImage` + `100ask_imx6ull-14x14.dtb` + `rootfs.ext4`）与 `r3c_bundle` 校验接口：生产端须由 SWUpdate `swupdate-public.pem`（或项目独立签发公钥，仅限 `/etc/smartgateway/` 根权限只读安装）校验整包 `sw-description.sig` 及各子镜像 `sha256`。
   - 禁止仅凭 `sha256` 校验放行升级；必须校验板型（`imx6ull-100ask`）、镜像展开上限（`rootfs.ext4 <= 槽位容量`）、最低版本防降级策略（可选安全计数器）与 `schema` 兼容声明。
2. **目标槽写前核验（隔离镜像优先）：**
   - 主机离线测试使用两个独立镜像文件（`slot1.ext4`、`slot2.ext4`、`emmc_env.img`、`data.ext4`）模拟双向升级（`2 -> 1` 与 `1 -> 2`），验证所有异常路径：错误签名、篡改子镜像、截断 `.swu`、错误目标槽（试图写当前运行槽）、空间不足、读回 SHA-256 不一致、`fsync` 失败、与并发 `ota_agent` `.ota.lock` 冲突。
3. **与当前 G3 / `post_build_g3.sh` 对接清单：**
   - 在 RootFS 安装受根权限 `0600` 保护的 `/etc/smartgateway/r3c-policy.conf`、公钥 `/etc/smartgateway/ota-public.pem` 及 G4 SWUpdate `sw-description` 钩子脚本。
   - G4 任何真机写盘前，必须先取得 **G1 板端恢复演练签收** 与 **G2-R3C 独立复审 `HOST-ACCEPTED`**。
