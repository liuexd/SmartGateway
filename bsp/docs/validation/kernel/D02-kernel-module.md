# D02 · 第一个 ARM Linux 内核模块：编译、传输与板端验证

> 状态：**核心验收通过**（开发者在真实 i.MX6ULL 板端的终端输出）；尚有少量收尾项。日期：2026-10-09。
>
> 本文证据来自开发者提供的 Ubuntu/板端命令输出与截图，不代表进行了自动化测试、重复加载压力测试或系统刷写。**D01 原厂恢复方案仍待验收。**

## 1. 源文件和构建环境

- 源码：[`bsp/drivers/hello/bsp_hello.c`](../drivers/hello/bsp_hello.c)
- 构建脚本：[`bsp/drivers/hello/Makefile`](../drivers/hello/Makefile)
- 编译产物：`bsp_hello.ko`（仅存在于开发者本地构建目录；不提交至 Git）。
- 内核源码/既有构建树：`~/Desktop/Linux-4.9.88`
- 目标架构：`ARCH=arm`
- 交叉编译工具链：Linaro `arm-linux-gnueabihf-` GCC 6.2.1。
- 构建配置实测：`CONFIG_MODULES=y`、`CONFIG_MODULE_UNLOAD=y`、`CONFIG_MODVERSIONS=y`；内核构建树存在 `Module.symvers`、`include/generated/autoconf.h` 和 `scripts/mod/modpost`。
- 编译前源码 Commit：[cbcf698](https://github.com/liuexd/SmartGateway/commit/cbcf6980550c5e1a56030350233ee0eaf21d6ddc)，新增 `.gitignore`、`Makefile` 和 `bsp_hello.c`。

实测 `file bsp_hello.ko`：

```text
ELF 32-bit LSB relocatable, ARM, EABI5 version 1 (SYSV), BuildID[sha1]=29043c057f8470d94b23de5d5682b6e23fa187ca, not stripped
```

实测 `modinfo bsp_hello.ko` 核心字段：

```text
description: First i.MX6ULL Linux kernel module
author:      SmartGateway-BSP
license:     GPL
depends:
vermagic:    4.9.88 SMP preempt mod_unload modversions ARMv7 p2v8
```

**注意**：`file` 与 `modinfo` 是静态检查，本身不保证一定能加载；本项目随后进行了板端加载验证。

## 2. 板端运行环境

在 100ASK i.MX6ULL 实机上：

```text
Linux 100ask 4.9.88 #1 SMP PREEMPT Fri Jun 5 19:16:01 CST 2026 armv7l GNU/Linux
Linux version 4.9.88 (ubuntu@ubuntu-VMware-Virtual-Platform) (gcc version 6.2.1 20161016 (Linaro GCC 6.2-2016.11) ) #1 SMP PREEMPT Fri Jun 5 19:16:01 CST 2026
```

- 用户：`uid=0(root) gid=0(root)`。
- 板端命令：`/sbin/insmod`、`/sbin/rmmod`、`/bin/dmesg` 存在。
- `/proc/modules` 中已有 `inv_mpu6050`、`100ask_ds18b20` 等驱动模块。
- Ubuntu 上 `adb devices -l` 已显示 `100ask_IMX6ULL device`，可通过 USB ADB 传输文件；ADB 输出的 `model:Nexus_4` 不是物理 PCB 型号证据。

## 3. ADB 传输及一致性

- Ubuntu 本地模块：`bsp/drivers/hello/bsp_hello.ko`
- 开发板校验路径：`/tmp/bsp_hello.ko`（板端 `/tmp` 为 `tmpfs`）。
- ADB 报告传输文件大小：`3740 bytes`。
- Ubuntu 与板端 `sha256sum` **均为**：

```text
38b44c6a9df54ec72bfb4ea0b567a2ec62abeda8c4b56ac9200707227b20b89d
```

传输过程曾误用目标路径 `/tmpbsp_hello.ko`（少一个斜杠，位于根目录而不是 `/tmp/`）；之后已在 `/tmp/bsp_hello.ko` 取得正确的 SHA-256 并成功加载。**尚未保存原错误路径是否已被移除的终端输出，需在收尾阶段检查 `/tmpbsp_hello.ko` 是否残留**。根目录所在文件系统为 eMMC ext4，不能把这次过程描述成“完全未触碰 eMMC”。

## 4. 板端 insmod / rmmod 实测

在开发板 root Shell 执行：

```sh
insmod /tmp/bsp_hello.ko
echo $?
cat /proc/modules | grep bsp_hello
dmesg | tail -n 20
rmmod bsp_hello
dmesg | tail -n 20
```

**实际加载日志**：

```text
[26535.892504] bsp_hello: loading out-of-tree module taints kernel.
[26535.916351] bsp_hello: module initialized
```

`echo $?` 在 `insmod` 后为 `0`。加载后 `/proc/modules`：

```text
bsp_hello 988 0 - Live 0x7f02d000 (O)
```

**实际卸载日志**：

```text
[26640.885385] bsp_hello: module exited
```

判定：

- **PASS**：`bsp_hello_init()` 入口确实执行，`insmod` 返回成功。
- **PASS**：`bsp_hello_exit()` 退出代码被调用，有对应内核日志。
- **PASS**：观察到 `/proc/modules` 加载态 `Live`。
- **INFO**：`loading out-of-tree module taints kernel` 与 `(O)` 表明加载了外部构建模块，不是本次加载失败。taint 状态不一定随 `rmmod` 清除。
- **待补充**：`rmmod` 后尚未记录 `/proc/modules` 中模块确实消失以及退出状态码；可在收尾时补做只读检查。

## 5. D02 验收结论与后续安全边界

| 项目 | 状态 |
| --- | --- |
| 外部模块 Kbuild 编译生成 ARM `.ko` | PASS |
| `modinfo` / `file` 检查 | PASS |
| ADB 推送和两端 SHA-256 校验 | PASS |
| `insmod` 初始化日志及加载态 | PASS |
| `rmmod` 退出函数日志 | PASS |
| 卸载后 `/proc/modules` 二次确认 | 待补 |
| 原厂恢复镜像和恢复路径核验（D01） | 待验收 |

### 收尾只读命令（在板端）

```sh
cat /proc/modules | grep bsp_hello
echo $?
ls -l /tmpbsp_hello.ko
```

`grep` 无输出且退出码 `1` 表示无该模块条目。错误路径若残留，先确认文件类型、大小后再移除；不要执行涉及系统分区的批量删除或刷写命令。

### 下一个阶段：D03

在不刷写原厂内核、DTB 和 eMMC 的前提下，先做 Platform Driver 框架与设备树的**只读分析**，再依照具体板级资源安全开展驱动实验。将 D01 的恢复验收保持为独立待办，不因 D02 已运行成功而自动视为完成。
