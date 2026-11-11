# System A/B OTA State Machine

## 1. 范围

系统 OTA 更新非活动槽位中的 Kernel、DTB 和 RootFS。第一阶段不更新 U-Boot 本体。已有应用级 OTA 保留，继续管理 SmartGateway 用户态 release。

## 2. 概念状态

```text
IDLE
  -> DOWNLOADED
  -> PACKAGE_VERIFIED
  -> INACTIVE_SLOT_SELECTED
  -> WRITING
  -> SLOT_VERIFIED
  -> PENDING_BOOT
  -> BOOTING_CANDIDATE
  -> HEALTH_CHECK
  -> CONFIRMED
```

失败分支：
- 下载/签名/哈希/兼容性校验失败：拒绝写入。
- 写非活动槽失败：保留当前活动槽，记录失败。
- 新槽启动失败或达到 boot_limit：U-Boot 选择 last_good_slot。
- Linux 启动但健康检查超时：由健康管理策略触发受控复位，使 Bootloader 能执行回退逻辑。
- 确认元数据更新中断：启动时使用冗余、CRC 正确且序号最新的状态记录恢复。

## 3. 升级包

建议包含 manifest、manifest signature、zImage、DTB、RootFS image 和 release metadata。

Manifest 至少约束：
- package format version；
- board/SoC compatibility；
- kernel version；
- rootfs format；
- slot image sizes；
- SHA-256；
- minimum supported U-Boot version；
- release version。

SHA-256 用于检测内容损坏；真实性依赖可信公钥验证的数字签名。签名公钥需要有明确的信任根和更新策略。

## 4. 写入约束

- 运行系统只能写非活动槽。
- 写入前验证槽位容量、分区边界、设备节点、挂载状态和镜像长度。
- 不允许活动槽与目标槽相同。
- 写入后执行 flush/sync、回读校验。
- 任一写入失败都不得设置 pending 状态。
- 设置 pending 前，最后确认槽位和启动元数据必须可恢复。
- OTA writer 不允许接受任意设备路径作为未经授权的写入目标；目标设备由经过校验的板级布局配置确定。

## 5. Boot Control Metadata

逻辑字段：
- format_version
- sequence_number
- active_slot
- pending_slot
- last_good_slot
- upgrade_available
- boot_attempts
- boot_limit
- slot versions
- successful flag
- CRC

持久化实现须明确冗余策略、写入顺序、回读、缓存刷新及掉电恢复。普通文件 rename 或单个环境变量不能自动保证底层块设备写入的断电原子性。

## 6. 健康确认

健康检查分层：
- 系统：Kernel 启动、RootFS 和 DATA 挂载；
- 驱动：GPIO、I2C/AP3216C 可用；
- 服务：Supervisor、Gateway、Agent、IPC；
- 网络：本地链路可用；外部服务器短时不可达不应单独判定为系统启动失败。

只有必需项全部通过，才将候选槽标记为 confirmed。确认记录应可追溯并具备掉电一致性。

## 7. 应用 OTA 共存

- 应用 OTA：只更新 DATA 上的 SmartGateway release。
- 系统 OTA：只更新非活动系统槽。
- 两者使用独立状态机和日志。
- 通过统一升级互斥机制防止并发升级。
- DATA 挂载失败时，依赖 DATA 的业务服务不得启动，也不得确认新系统。
- 回滚系统时保留 DATA，但必须考虑新旧系统对 DATA schema 的兼容性；必要时使用向后兼容迁移或版本化数据格式。

## 8. 必须测试的故障

- 非法签名、错误哈希、包截断、错误板型；
- 目标槽空间不足；
- 写入中断、回读校验失败；
- pending 元数据写入期间断电；
- Kernel/DTB 加载失败；
- RootFS 挂载失败；
- 启动后服务健康检查失败；
- 健康确认前复位；
- 确认记录写入期间断电；
- DATA 挂载失败；
- 应用 OTA 与系统 OTA 并发。

本文件是设计基线。所有测试在真实板端完成并保存日志后，才可更新为 PASS。
