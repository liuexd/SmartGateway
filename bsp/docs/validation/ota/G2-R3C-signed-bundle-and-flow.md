# G2-R3C 继续工作：隔离 DATA、签名事务和 Gateway 流量观察

日期：2026-11-17；基线 `ce203b1be02bcc74396d08fa1cb5b5d495b009a0`，`feature/imx6ull-system-ab-ota`。工作在独立 Git 工作区和用户拥有的临时文件中完成。历史阶段记录见 [前一增量报告](G2-R3C-host-increment.md)。

**验收结论：G2-R3C HOST-ACCEPTED 仍为 BLOCKED。** 四项均有新增证据或前置实现，但完整系统升级者、真实 DATA 布局、可信健康服务和最终确认决策尚未接通。N09/N10 生产决策链未执行；`sgw_bootctl confirm` 仍然默认拒绝。没有修改目标板、真实 eMMC、U-Boot 环境、正式 Gateway 或 Watchdog；没有推送。

## 1. 四项工作与实际边界

| 事项 | 本轮完成 | 尚未闭合 |
|---|---|---|
| 共享 DATA | 通过普通用户 FUSE 挂载独立 ext4 镜像，真实读取、写入并重新挂载；用生产共享的内容检查函数验证 schema、空间、只读和符号链接拒绝。应用 OTA 快照支持把原共享锁保持到调用者显式释放 | FUSE 是 `fuseblk`，不是目标板的 ext4 块设备，未证明生产 `r3c_data_inspect` 正常块设备身份路径、槽位/扇区排除、实际 DATA 硬件身份或板端跨槽；系统确认尚未持锁调用 |
| 可信事务 | 新增试验性 RSA/SHA-256 包清单签名和镜像摘要校验；使用系统随机源创建 ID，在受保护目录中独占创建 VERIFIED 记录并执行 fsync；拒绝未结束记录、篡改、错误密钥、锁冲突及写入/fsync 失败 | 实际 SWUpdate `.swu`/签名格式、可信密钥策略、Kernel/DTB、设备布局、系统写入与读回、生产调用者认证、boot_id 绑定、消费/抗重放和确认原子性均未集成。试验包不是可安装的系统 OTA 包 |
| Gateway 业务 | 在真实 Server Link worker 成功发送现有 DATA 后被动记录近期进度；受控的本地查询复用现有 peer PID/uid/starttime 和随机 nonce。隔离测试确认没有额外上游探针数据，普通业务照常转发；无近期流量返回 UNKNOWN | 证据只说明本进程向 TCP 套接字成功发送了 DATA，不证明远端应用接收，也可能是队列中先前数据；无数据或外网不可达时无法证明健康。5 秒阈值是试验值，不是产品策略；没有接入生产确认 |
| N09/N10 | 现有生产确认拒绝路径和新签名记录拒绝/持久化失败路径有主机测试 | 真实 Gateway、DATA、已安装系统镜像、受信事务与生产健康确认的共同路径仍不存在。N09 正常确认、N10 消费后重放和确认持久化故障均 **BLOCKED / NOT RUN** |

## 2. 隔离 DATA 与事务实现

- `r3c_data_inspect_contents` 只检查一个已打开目录的属主/权限、只读状态、空间与候选 `schema=1\n` 文件；它**不认证设备或挂载**。原 `r3c_data_inspect` 先执行真实 mountinfo 和块设备检查，再调用内容检查。候选 schema 尚未批准或部署。
- `r3c_app_ota_inspect_locked` 成功后把已锁定的 fd 交还调用方，直至调用方释放才能让遵循既有 `.ota.lock` 的应用 OTA 获取排他锁；这不等同系统/应用 OTA 全局互斥，也未保证同一锁覆盖所有写入方。
- `r3c_bundle_record` 接收由调用方验证的目录和公钥 fd，要求属主/权限及普通文件条件，校验固定试验 manifest 和 RSA 密钥（至少 2048 位）对 manifest 的 SHA-256 签名，再计算 rootfs 文件长度和摘要。记录里的 `package` 定义为 `SHA256(manifest || signature)`，并非原厂 SWU 整包摘要；`image` 是试验 rootfs 文件摘要。
- 试验 manifest 的板型字符串 `imx6ull-100ask` 和文件布局只是测试契约，不代表厂商包、设备鉴权、升级元数据或正式发布格式。记录状态仅为 VERIFIED；创建不会写目标槽、设置 pending 或确认。
- 调用方必须保证公钥、目录和 DATA 身份可信，且实际安装必须对已验证文件描述符或写后回读结果重新验证。当前代码未连接生产升级服务，也没有在健康确认期间统一持锁；不能将返回成功解释成事务来源和安装已可信。
- fsync 失败测试以测试专用 `LD_PRELOAD` 故障库注入，仅运行在隔离 CLI 进程；报告拒绝并检查该进程没有留下记录。它不证明真实掉电、块设备缓存或崩溃后的持久化收敛。

## 3. Gateway 观察语义

现有 `SGW-PROBE/1` 组件查询不变；本次增加只在原 root 限制 Unix socket 内使用的 `SGW-FLOWX/1` 查询。被动记录发生在已有 Server Link worker 的真实队列消息成功发送之后，解析为 DATA 后更新进程内单调时间；无额外消息进入生产队列或北向上游。若没有新 DATA、超过候选时限或时间异常，查询不返回观察成功。

这是**传输发送进度诊断**，不能直接作为必需业务 HEALTHY。TCP send 成功不证明服务端解析/接收；队列中滞留的旧业务也可能被发送。要求完整业务链时，仍须设计可关联输入、处理、发送和必要的服务端确认，同时明确无流量和外网故障策略。该实现也需要负载、线程卡顿、进程替换和正式产品超时评审。

## 4. 主机回归与原始输出

| 操作 | 实际结果 | 日志 |
|---|---|---|
| `make -C bsp/apps/boot_ab clean` 后 `make -C bsp/apps/boot_ab test arm-check arm-r3c-check` | 状态机 100/100、环境 31/31、健康模型 195/195、设备 21/21、网络 5/5、字段绑定 16/16；DATA 测试 7/7、签名事务测试 5/5 | [verified-boot.log](g2-r3c2-host-logs/verified-boot.log) |
| `SGW_TEST_FUSE2FS=/tmp/sgw-r3c2-rOMqb4/fuse-pkg/usr/bin/fuse2fs python3 -m unittest -v tests.test_g2r3c_fuse_image` | 1/1；实际临时 ext4 镜像经 FUSE 挂载、重挂、只读/缺空间/schema 异常拒绝。测试后已卸载 | [fuse-final2.log](g2-r3c2-host-logs/fuse-final2.log) |
| `UBOOT_AB_BUILD_DIR=/tmp/sgw-g1g2-audit-c2TLcv/repo/build/uboot-ab python3 -m unittest -v tests.test_g1_backup_guard tests.test_g2_uboot_ab tests.test_g2r3_health` | 25/25，含确认默认拒绝和冗余环境模拟 | [verified-g1g2.log](g2-r3c2-host-logs/verified-g1g2.log) |
| 测试专用 Gateway 构建并运行 `python3 -m unittest -v tests.test_g2r3b_gateway_probe` | 2/2；包括未观察 DATA 拒绝、真实 DATA 转发、没有额外探针上报和过期拒绝；构建日志仅去除行尾空白 | [verified-flow.log](g2-r3c2-host-logs/verified-flow.log)、[flow-build.log](g2-r3c2-host-logs/flow-build.log) |
| `make -C bsp/apps/service_supervisor test` | 58/58、35/35 | [supervisor.log](g2-r3c2-host-logs/supervisor.log) |
| `make -C bsp/apps/ota_agent test-supervisor test-supervisor-s2`，输出二进制置于独立临时目录 | 145/145、44/44 | [ota.log](g2-r3c2-host-logs/ota.log) |
| `make test` | 10 个 Gateway / Server 单元测试程序通过 | [core.log](g2-r3c2-host-logs/core.log) |
| `make gateway arm arm-check` | Gateway ARMv7 hard-float 编译及 ABI 检查通过 | [gateway.log](g2-r3c2-host-logs/gateway.log) |
| `python3 tests/test_ota_health_e2e.py` | 旧 L2 在隔离上游通过；它不是生产无副作用探针 | [ota-e2e.log](g2-r3c2-host-logs/ota-e2e.log) |

FUSE 工具通过 `apt download` 取得 `fuse2fs 1.47.2-3ubuntu4 amd64` 并仅解包到临时目录，未安装系统软件；测试不使用 sudo。镜像仅用于主机测试，不表明其默认 ext4 特性兼容目标板。

ARM 交叉编译只证明构建和 ELF 依赖：新增签名测试程序依赖 `libcrypto.so.1.1` / `libc.so.6`、最高 glibc 符号 2.25；未在开发板执行。生产 bootctl 的默认拒绝逻辑未改变，未部署任何测试二进制。

## 5. N09/N10 的最小剩余链路

1. 批准 DATA 的硬件身份、槽位/扇区排除和 schema 兼容策略；在允许安全隔离块设备挂载的环境覆盖真实块设备成功与失败路径。
2. 完成受信系统升级包和写入器，将签名验证、实际写入/回读、可信事务、全局互斥与 boot_id/槽位绑定连起来；当前试验 record 不得直接授权升级。
3. 提供不会污染业务且可证实完整处理链的 Gateway 状态及可信 GPIO/AP3216C/本地网络采样，并将其与 DATA、事务和稳定观察绑定到同一启动实例。
4. 持锁调用同一生产确认决策并进行可靠持久化/回读，实际完成 N09、N10（崩溃、超时、旧证据、重复确认和落盘失败）。只有全量正常/拒绝回归通过才评审 HOST-ACCEPTED；G1/G2 板端需另行恢复与真机验收。
