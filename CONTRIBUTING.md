# SmartGateway 开发与 Commit 规范

本文件约定后续代码提交、测试和阶段验收方式，**不改变历史 Commit**。适用于 M7-8 和今后的可选扩展。

## 1. 核心原则

- 一次 Commit 聚焦一个可解释、可回退的变更：新功能、缺陷修复、单测或文档。
- 先检查改动再提交：避免把 build/、日志、临时文件、个人配置或编译生成的二进制纳入版本管理。
- 功能实现不等于验收成功；单测、集成测试、异常测试的完成情况要如实记录。
- 串口 fd、WiFi client fd、Server fd 均需有明确的线程所有权；共享状态要明确 mutex / condition variable 边界。
- M7-8 真机完成前，不把 Mock WiFi Node 写成已完成 STM32 + ESP8266 实物联调。

## 2. 推荐 Commit 标题格式

~~~text
<type>(<scope>): <short imperative summary>
~~~

| type | 场景 |
| --- | --- |
| feat | 新功能 |
| fix | 缺陷修复 |
| test | 单测、集成测试或测试工具 |
| refactor | 不改变对外行为的代码重构 |
| docs | README、设计文档、开发日志 |
| build | Makefile、构建与工具链 |
| chore | 杂项维护、清理 |
| perf | 性能优化 |

常见 scope：gateway、wifi、bluetooth、device-manager、server、protocol、node02、integration、readme。英文简洁标题便于 GitHub 统一展示；正文可写中文。

**示例（未来提交建议，不代表已经实现）：**

~~~text
feat(node02): add STM32 ADC sampling for LIGHT_RAW
feat(wifi): connect ESP8266 to gateway TCP listener
fix(gateway): handle WiFi reconnect without stale device state
test(integration): verify Bluetooth and WiFi link isolation
test(stability): record dual-node long-running results
docs(readme): document M7-8 hardware validation
~~~

若需要保留阶段编号，请放在 Commit 正文，而非每条都堆在标题：

~~~text
feat(node02): add STM32 ADC sampling for LIGHT_RAW

Milestone: M7-8
- Sample sensor values through ADC.
- Send LIGHT_RAW over UART.
- Tested: local UART output verified.
~~~

## 3. 每次提交前检查

~~~bash
git status
git diff
git diff --cached --check
git diff --cached --stat
git diff --cached
~~~

确认无意外修改、行尾空白、调试日志及编译产物。按需指定文件暂存，避免直接使用 git add . 将无关文件一并提交。

~~~bash
git add -- path/to/changed_file.c path/to/changed_file.h
git diff --cached --check
git commit -m "fix(gateway): handle WiFi reconnect safely"
git push origin main
~~~

可将功能开发放在独立分支并通过 PR 合并；直接推送 main 时尤其应确保通过对应测试。

## 4. 构建与测试标准

~~~bash
make clean
make all
make test
~~~

按改动范围增加：

~~~bash
make test-tcp
make test-asan
make test-tsan
~~~

- make test 目前登记 **10 个单元测试程序**。
- test-tcp 依赖本机 TCP 服务端测试工具和 nc。
- ASan / TSan 需要操作系统与编译器提供相应支持；两者分开运行。
- 涉及网络或多线程的改动，应额外手动复测断线、重连、队列 shutdown、Ctrl+C、fd / thread 回收。
- 测试失败时先修复，不要在 Commit 消息中声称通过。

## 5. M7-8 推荐拆分

以小步可验收为原则，推荐拆成以下独立提交（仅为计划）：

1. **feat(node02): acquire LIGHT_RAW from STM32 ADC** — ADC 与串口数值验证。
2. **feat(wifi): establish ESP8266 TCP connection** — AT / UART / WiFi / TCP 链路验证。
3. **feat(node02): upload sensor readings to gateway** — 真实 NODE02 DATA 上传。
4. **test(integration): verify concurrent Bluetooth and WiFi nodes** — 双链路、断线与恢复。
5. **test(stability): validate long-running gateway resources** — CPU、RSS、fd、线程数与长稳结果。
6. **docs(readme): complete M7-8 acceptance and usage guide** — 更新硬件清单、运行步骤、验收结果。

如果 MCU 固件目前保存在其他仓库，应在 README 中提供实际链接，不要在当前仓库虚构固件路径。未来新增固件目录时再对应修改目录树。

## 6. 每个阶段的验收记录模板

~~~text
Milestone: M7-8
Scope: ADC / UART / WiFi / Gateway / Server
Changes:
- ...
Verification:
- make clean && make all: PASS / FAIL / NOT RUN
- make test: PASS / FAIL / NOT RUN
- 双节点联调: PASS / FAIL / NOT RUN
- 网络异常/重连: PASS / FAIL / NOT RUN
- 资源监测: PASS / FAIL / NOT RUN
Known limitations:
- ...
~~~

## 7. 开发记录与版本管理

- 每个阶段完成后更新 [CHANGELOG.md](CHANGELOG.md)，链接到对应 Commit。
- README 只描述目前已在仓库中实现的能力及经过验证的结果。
- 发布正式 Tag（如首次完整 M7 版本）前，确认最终验收清单已通过；不要用 Tag 暗示未完成的硬件测试已经完成。
- **不要改写既有 M5 / M7-1～M7-7 提交历史**。已有短 Commit 标题可以通过 CHANGELOG 补充背景和具体工作。
