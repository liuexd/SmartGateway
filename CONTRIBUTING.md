# SmartGateway 开发与文档维护规范

本文件适用于 SmartGateway、i.MX6ULL BSP 和系统级 OTA 后续开发。目标是让代码、设计、验证记录和 README 始终反映仓库真实状态。

## 1. 核心原则

- 一次 Commit 聚焦一个可解释、可回退的变更：功能、缺陷修复、测试、构建或文档。
- 实现、测试通过、板端验收是不同状态，必须分别描述。
- README 只描述当前仓库中已经实现且有证据支持的能力。
- 设计方案不得写成已实现功能；计划不得写成验收结果。
- 不提交 build 产物、日志、临时文件、个人配置、密钥或未经许可的厂商镜像。
- 修改启动链、eMMC 分区、U-Boot 环境或系统镜像前，必须满足恢复就绪检查表的放行条件。

## 2. 分支约定

| 分支 | 用途 |
|---|---|
| `main` | 项目稳定主线 |
| `feature/bsp-imx6ull` | i.MX6ULL BSP 驱动、Agent、调试和应用级 OTA 基线 |
| `feature/imx6ull-system-ab-ota` | Kernel / Buildroot 定制、启动链与系统级 A/B OTA 扩展 |

新工作优先在对应功能分支完成。不要将系统级 A/B OTA 的未验证设计描述为已交付功能。合并前核对目标分支、测试范围和文档状态。

## 3. Commit 标题

格式：

```text
<type>(<scope>): <short imperative summary>
```

| type | 场景 |
|---|---|
| feat | 新功能 |
| fix | 缺陷修复 |
| test | 单元、集成或故障注入测试 |
| refactor | 不改变外部行为的重构 |
| docs | README、设计文档、验收记录 |
| build | Makefile、工具链和构建系统 |
| chore | 仓库维护 |
| perf | 有测量依据的性能优化 |

推荐 scope：gateway、bsp、gpio、ap3216c、ota、supervisor、uboot、kernel、buildroot、docs。

示例：

```text
fix(gpio): release irq resources on probe failure
docs(bsp): document i.MX6ULL recovery constraints
test(ota): cover interrupted inactive-slot update
build(kernel): reproduce vendor kernel configuration
```

## 4. 文档目录规则（固定结构）

后续文档按内容类型归档，不再创建 D17、D18 或 D26 等日期/阶段平铺目录。

```text
bsp/docs/
├── README.md                 # 唯一文档导航入口
├── system-ab/                # 当前系统定制与 A/B OTA 设计/实施
├── reference/                # 长期技术参考和教程
├── validation/               # 已执行的验收记录
└── archive/                  # 被新基线替代的历史资料
```

具体规则：

1. **当前设计与实施文档**放入 `bsp/docs/system-ab/`。按主题命名，例如 `07-kernel-build-reproduction.md`、`08-buildroot-rootfs.md`。只有确实新增独立主题时才新增文件，不为每日进度创建文档。
2. **历史验收记录**放入 `bsp/docs/validation/<module>/`。模块目录沿用现有分类：platform、kernel、gpio、sensors、debug、build、ota。
3. **可重复使用的知识资料**放入 `bsp/docs/reference/<topic>/`。
4. **被新资料替代但仍有历史价值的文件**放入 `bsp/docs/archive/`，并在 archive README 说明替代关系。
5. 新文档必须从 `bsp/docs/README.md` 或所属模块 README 建立入口。禁止新增未被索引的孤立文档。
6. 不随意改名或搬迁既有验收记录；确有必要时，同一 Commit 更新所有引用链接。
7. 不恢复旧 `ROADMAP.md` 的 Dxx 时间排期体系。当前任务顺序维护在 `system-ab/README.md`，仅记录依赖、状态和验收门槛。

## 5. 验收记录模板

每份新验收报告至少说明：

```text
# <主题> 验收记录

状态：DESIGN / IMPLEMENTED / HOST-TESTED / BOARD-TESTED / BLOCKED
平台：板卡型号、SoC、Kernel、RootFS
范围：本次变更包含与不包含的内容

## 变更
- ...

## 验证环境与命令
- ...

## 结果
- 构建：PASS / FAIL / NOT RUN
- 单元测试：PASS / FAIL / NOT RUN
- 板端验证：PASS / FAIL / NOT RUN
- 故障场景：PASS / FAIL / NOT RUN

## 证据
- 关键命令输出、日志、版本或测试计数

## 已知限制
- ...
```

状态应选择最严格且有证据支持的等级。HOST-TESTED 不能写成 BOARD-TESTED；编译成功不能写成完整功能验收通过。

## 6. 提交前检查

```bash
git status --short
git diff --check
git diff --stat
git diff
```

暂存后再次检查：

```bash
git diff --cached --check
git diff --cached --stat
git diff --cached
```

按文件路径精确暂存，避免将无关文件、生成物或本地配置提交。

## 7. 构建与测试

根据实际改动选择测试，不得将未执行项目标记为 PASS。

网关主机端基础回归：

```bash
make clean
make all
make test
```

按需运行：

```bash
make test-tcp
make test-asan
make test-tsan
```

涉及 BSP / OTA 时，还应记录对应 ARM 交叉编译、目标板版本、实际命令和故障注入范围。不要用主机模拟测试替代板端结论。

## 8. README、CHANGELOG 与文档同步

- 根目录 README 描述整个 SmartGateway 当前真实能力和开发方向。
- `bsp/README.md` 描述 BSP 平台、驱动、系统架构与当前状态。
- `bsp/docs/README.md` 是 BSP 文档唯一总索引。
- `CHANGELOG.md` 记录有意义的代码/工程里程碑，不记录每次小型文字调整。
- 完成一个可独立验收的功能后，同步更新相应 README、验收记录和 CHANGELOG。
- 只修改文档结构时，不要顺手改写无关代码或历史提交。
