# D25-1 · ARM 交叉编译与板端兼容性验收

> 项目：SmartGateway-BSP / 100ASK i.MX6ULL
> 分支：`feature/bsp-imx6ull`
> 记录日期：2026-11-06
> 阶段性质：**构建流程建立 + 静态兼容性验收**；板端实机验证见 D25-1B

> **更新（2026-11-06，D25-1B 后）**：本文档记录时板端不可达，全部板端项标记
> BLOCKED。[D25-1B](D25-1B-board-environment-recheck.md) 已恢复连接并完成
> 板端实测。**本文档中被 BLOCKED 的项现已有实测结论**：
>
> | 本文档中的 BLOCKED 项 | D25-1B 实测结果 |
> | --- | --- |
> | 板端 glibc 版本（原为间接证据） | **2.30**（实测 `/lib/libc.so.6`） |
> | 板端依赖库存在性 | **全部齐备**，OpenSSL 1.1.1g / json-c 4.0.0 |
> | OTA Agent 在板端执行 | **PASS**（verify 与 4 个负向用例） |
> | Gateway 隔离启动 | **PASS**（19000 端口，完整生命周期） |
> | 持久化目录可用性 | **PASS**（重启后 inode/md5 一致） |
>
> 本文档保留原记录与推理过程，用于说明**在无板端条件下能得出何种结论、
> 以及哪些必须留给板端**。阅读当前结论请以 D25-1B 为准。

## 0. 结论摘要（先看这里）

| 验收项 | 结果 | 依据 |
| --- | --- | --- |
| ARM Gateway 构建无错误，生成正确 ARM ELF | **PASS** | 第 4 节 |
| ARM OTA Agent 构建无错误，生成正确 ARM ELF | **PASS** | 第 4 节 |
| ABI 与动态库与板端环境兼容 | **PASS（静态）** | 第 5 节：GLIBC 需求 ≤ 2.28 < 板端 2.30 |
| OTA Agent 基础功能可在目标板执行 | **BLOCKED** | 板端不可达；仅完成 QEMU 用户态模拟，见第 7 节 |
| Gateway 基础功能可在隔离环境启动 | **部分通过** | 宿主 QEMU 隔离启动 PASS；板端 **BLOCKED** |
| 原有 x86 构建与测试无回归 | **PASS** | 第 8 节 |
| 正式 Gateway 业务不受影响 | **PASS（未接触）** | 全程未访问板端，第 9 节 |
| 文件或进程泄漏 | **0** | 第 9 节 |
| 持久化目录可用性有实际检查证据 | **BLOCKED** | 需要板端；第 6 节给出待执行脚本 |

**一句话总结**：交叉编译链路已经打通并有静态证据；但**本机无法访问目标板**（网段、ADB、串口三者均不可用），
因此第 1、3 节的全部板端实测项都是 **BLOCKED**，不是 PASS。

**不得**把本报告的 QEMU 模拟结果当作板端 PASS。QEMU 用户态模拟无法证明：
VFP 指令在真实硬件上的行为、`/opt` 等目录是否跨重启持久、正式 Gateway 并存时的端口占用处理。

---

## 1. 目标板硬件与软件环境

### 1.1 本轮实际可核验到的部分

**关键限制**：本机与目标板**无任何连接通道**。第 10 节记录了完整证据。
因此 1.2 节的环境数据来自仓库既有实测记录（D01 / D16），而**不是本轮重新采集**。

| 字段 | 值 | 来源 |
| --- | --- | --- |
| SoC | NXP i.MX6ULL rev1.1，单核 Cortex-A7，ARMv7-A | D01 实机 `uname`/`dmesg` |
| 内存 | 512 MiB DDR | D01 |
| 存储 | eMMC S40004，约 3.64 GiB | D01 |
| Kernel | Linux 4.9.88，ARMv7，PREEMPT | D01 |
| Rootfs | Buildroot 2020.02（`2020.02-g65177d4`），ext4 | D01 |
| 动态链接器 | `/lib/ld-linux-armhf.so.3 -> ld-2.30.so` | **D16 实测** |
| 内核控制台 | `ttymxc0,115200` | D01 |
| 板端 IP（历史） | `192.168.10.3` | D06 / D09 / D16 |

`ld-2.30.so` 是本轮工具链选型的决定性依据：它说明板端 glibc 为 **2.30**。

### 1.2 本轮未能核验的部分（BLOCKED）

以下项目**必须**在板端执行，本轮因无连接通道而全部未执行：

- CPU flags、`/proc/cpuinfo` 中的 VFP 能力。
- 板端 `libssl.so.1.1` / `libcrypto.so.1.1` / `libjson-c.so.4` 是否真实存在及版本。
- 目标板当前运行的 Gateway 版本、进程、监听端口、启动方式。
- `/tmp`、rootfs 与候选持久化目录的空间、挂载类型、权限。
- OTA 目标安装目录的持久性（**必须重启验证**）。

**特别声明**：任务书要求"不得直接假定已有的 Linaro GCC、OpenSSL 或 json-c 与当前目标板完全兼容，必须验证"。
本轮对该要求做到了**工具链侧**的验证（用与板端同源的 Buildroot SDK，sysroot 内为 glibc 2.30 + OpenSSL 1.1.1g + json-c 0.13.1），
但**板端库的实际存在性仍未经实测**。这是 D25-2 的首要前置条件。

---

## 2. 交叉编译器、sysroot 与第三方库的选择依据

### 2.1 两个候选工具链的对比

宿主机上存在两套 ARM 工具链，本轮对二者做了实测对比：

| 项目 | Linaro GCC 6.2.1-2016.11 | **Buildroot 2020.02 SDK** |
| --- | --- | --- |
| 路径 | `~/Desktop/ToolChain/gcc-linaro-6.2.1-.../` | `~/Desktop/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot/` |
| GCC 版本 | 6.2.1 | **7.5.0** |
| sysroot 内 glibc | **2.23** | **2.30** |
| `ld-linux-armhf.so.3` | → `ld-2.23.so` | **→ `ld-2.30.so`** |
| OpenSSL | **不存在** | **1.1.1g** |
| json-c | **不存在** | **0.13.1** |
| 默认 `-mcpu` | 未指定 | **cortex-a7** |
| 默认 `-mfpu` | 未指定 | **neon-vfpv4** |

### 2.2 选择结论

**采用 Buildroot 2020.02 SDK**，理由：

1. **动态链接器版本精确匹配**。板端实测为 `ld-2.30.so`，SDK 的 sysroot 正是 2.30。Linaro 是 2.23，
   两者虽同为硬浮点 ARMv7，但 glibc 版本不一致会带来两类风险：
   - 用 2.23 的头文件编译、链接 2.30 板端库，可能用到只在 2.24+ 才有的符号语义；
   - 反向（本仓库历史产物）虽能运行，但缺少 `GLIBC_2.24~2.30` 提供的优化与修复。

2. **第三方库自洽**。OTA Agent 依赖 OpenSSL 与 json-c。Linaro sysroot **完全不含**这两个库
   （实测 `find` 无结果），若强行使用只能指向宿主机 x86 的 `/usr/include` 与 `/usr/lib`，
   这正是任务书明令禁止的"宿主机 x86 库混入 ARM 程序"。

3. **与板端 rootfs 同源**。Buildroot SDK 由同一份 Buildroot 配置导出，库版本（OpenSSL 1.1.1g、
   json-c 0.13.1）即板端版本，避免 ABI 差异。

4. **目标内核标记匹配**。Buildroot 工具链产出的 ELF 标记为 `for GNU/Linux 4.9.0`，
   而 Linaro 产出为 `for GNU/Linux 2.6.32`——前者与板端 4.9.88 对应。

### 2.3 编译参数

```text
-mcpu=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4 -marm
--sysroot=<SDK>/arm-buildroot-linux-gnueabihf/sysroot
```

`-mcpu=cortex-a7` 与 i.MX6ULL 的单核 Cortex-A7 精确对应；`-mfloat-abi=hard` 匹配板端
`ld-linux-armhf.so.3`（hf = hard-float）。

### 2.4 防止宿主机库混入的机制

`bsp/apps/ota_agent/Makefile` 中做了三重隔离：

```make
ARM_PKG_ENV := \
	PKG_CONFIG_LIBDIR=$(ARM_SYSROOT)/usr/lib/pkgconfig:$(ARM_SYSROOT)/usr/share/pkgconfig \
	PKG_CONFIG_SYSROOT_DIR=$(ARM_SYSROOT)
```

1. 使用 SDK 自带的 `pkg-config` 包装脚本（其内部已把 `PKG_CONFIG_SYSROOT_DIR` 指向目标 sysroot）。
2. 再显式覆盖 `PKG_CONFIG_LIBDIR`，即使调用者 shell 里设了宿主机变量也不会读到 `/usr/lib/pkgconfig`。
3. 编译与链接均带 `--sysroot=`，`<>` 头文件与 `-l` 库全部解析到目标 sysroot。

**验证**：`arm-info` 目标的输出确认 `-I`/`-L` 全部指向 sysroot 内部（见 4.3 节）。

---

## 3. 构建流程

### 3.1 新增目标一览

`Makefile`（仓库根）：

| 目标 | 作用 |
| --- | --- |
| `make arm` | 交叉编译 Gateway → `build/arm-d25/smart_gateway` |
| `make arm-check` | 只读检查 ARM ELF（架构/解释器/依赖/ABI/glibc 需求） |
| `make arm-info` | 打印生效的交叉编译参数 |
| `make clean-arm` | 只删 ARM 产物 |

`bsp/apps/ota_agent/Makefile`：

| 目标 | 作用 |
| --- | --- |
| `make arm` | 交叉编译 OTA Agent + ota_verify → `build/arm-d25/` |
| `make arm-ota_agent` / `arm-ota_verify` | 单独构建 |
| `make arm-check` | 只读 ELF 检查 |
| `make clean-arm` | 只删 ARM 产物 |

### 3.2 输出目录隔离

| 产物 | 路径 | 架构 |
| --- | --- | --- |
| x86 Gateway | `build/smart_gateway` | x86-64 |
| x86 Server | `build/gateway_server` | x86-64 |
| **ARM Gateway** | `build/arm-d25/smart_gateway` | ARM |
| **ARM OTA Agent** | `build/arm-d25/ota_agent` | ARM |
| **ARM ota_verify** | `build/arm-d25/ota_verify` | ARM |

ARM 产物使用独立的 `build/arm-d25/` 前缀，与 x86 产物**零重叠**。

> **遗留行为（已知）**：根 `Makefile` 的 `clean` 会 `rm -rf build`，因此会连带删除 `build/arm-d25/`。
> 这不是覆盖，但会误删 ARM 产物。使用 `make clean-arm` 可只清 ARM；若需保留 ARM 产物，
> 重建即可（约 3 秒）。该行为本轮**未修改**，以保持 `clean` 语义不变。

### 3.3 保留原有 x86 流程

- 根 `Makefile` 的 `all` / `gateway` / `server` / `mock_node` / `mock_wifi_node` /
  `test` / `test-tcp` / `test-asan` / `test-tsan` **全部未改动**。
- `bsp/apps/ota_agent/Makefile` 的 `ota_verify` / `ota_agent` / `ota_agent_x86`
  编译参数与产物名**保持不变**，仅把头文件依赖列表补入新增的 `ota_json_compat.h`。

---

## 4. 构建结果

### 4.1 工具链与 sysroot 实测

```text
$ .../arm-buildroot-linux-gnueabihf-gcc --version | head -1
arm-buildroot-linux-gnueabihf-gcc.br_real (Buildroot 2020.02-gee85cab) 7.5.0

$ strings .../sysroot/lib/libc.so.6 | grep -oE "stable release version [0-9.]+"
stable release version 2.30.

$ readlink .../sysroot/lib/ld-linux-armhf.so.3
ld-2.30.so
```

### 4.2 构建命令与结果

```sh
make arm                       # 根目录：Gateway
cd bsp/apps/ota_agent && make arm   # OTA Agent + ota_verify
```

两者均**零警告、零错误**（`-Wall -Wextra -Werror`）。

### 4.3 生效参数（`make arm-info`）

```text
ARM_CC         = .../arm-buildroot-linux-gnueabihf-gcc
ARM_SYSROOT    = .../arm-buildroot-linux-gnueabihf/sysroot
ARM_BUILD_DIR  = ../../../build/arm-d25
ARM_CFLAGS     = -std=c11 -O2 -g -Wall -Wextra -Werror -pthread \
                 -mcpu=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4 -marm \
                 --sysroot=.../sysroot
ARM_PKG_CFLAGS = -I.../sysroot/usr/include -I.../sysroot/usr/include/json-c
ARM_PKG_LIBS   = -L.../sysroot/usr/lib -lssl -lcrypto -ljson-c
```

`-I` 与 `-L` **全部指向 sysroot 内部**，无任何 `/usr/include` 或 `/usr/lib` 宿主机路径。

### 4.4 产物清单

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `build/arm-d25/smart_gateway` | 199 984 B | `1beb5d2f398f31a8f786efcba79987d1eed824f91749d94b9e0d93a8c92b6303` |
| `build/arm-d25/ota_agent` | 152 160 B | `876ddf5d4d74f131cd3607291473704a59b8e588f146e2e441214e5aefb69c6b` |
| `build/arm-d25/ota_verify` | 27 872 B | `961a0a5e8cca862bc7f39ffa6c75ec0488e1a0b9f908ade1f10e4aafc960d65d` |

---

## 5. ARM ELF、动态链接器与依赖验证

### 5.1 ELF 架构 — PASS

```text
$ file build/arm-d25/smart_gateway
ELF 32-bit LSB executable, ARM, EABI5 version 1 (SYSV), dynamically linked,
interpreter /lib/ld-linux-armhf.so.3, for GNU/Linux 4.9.0, with debug_info, not stripped

$ file build/arm-d25/ota_agent
ELF 32-bit LSB executable, ARM, EABI5 version 1 (SYSV), dynamically linked,
interpreter /lib/ld-linux-armhf.so.3, for GNU/Linux 4.9.0, with debug_info, not stripped
```

- 32 位 ✓ / ARM ✓ / EABI5 ✓ / 小端 ✓
- 目标系统标记 `GNU/Linux 4.9.0`，与板端 4.9.88 对应
- 对比：仓库历史 `build/arm/smart_gateway`（Linaro 编译）标记为 `GNU/Linux 2.6.32`

### 5.2 动态解释器 — PASS

```text
[Requesting program interpreter: /lib/ld-linux-armhf.so.3]
```

`hf` 后缀即 hard-float，与板端 `ld-linux-armhf.so.3 -> ld-2.30.so` 匹配。三个产物一致。

### 5.3 ABI 属性 — PASS

```text
Tag_CPU_arch: v7
Tag_CPU_arch_profile: Application
Tag_FP_arch: VFPv4
Tag_ABI_VFP_args: VFP registers
```

- `v7` ← 板端 ARMv7 ✓
- `VFPv4` ← i.MX6ULL Cortex-A7 的 FPU 为 VFPv4 ✓
- `VFP registers` ← 硬浮点调用约定 ✓

对比：仓库历史 `build/arm/` 产物为 `VFPv3-D16`（Linaro 默认），偏保守但同样可运行。

### 5.4 动态库依赖 — PASS（静态）

| 产物 | NEEDED |
| --- | --- |
| `smart_gateway` | `libpthread.so.0`, `libc.so.6` |
| `ota_agent` | `libssl.so.1.1`, `libcrypto.so.1.1`, `libjson-c.so.4`, `libpthread.so.0`, `libc.so.6` |
| `ota_verify` | 同 `ota_agent` |

**无 RPATH / RUNPATH** → 运行时使用板端默认 `/lib`、`/usr/lib` 搜索路径，不会指回宿主机目录。

### 5.5 glibc 符号版本兼容性 — PASS（静态）

| 产物 | 最高 GLIBC 符号需求 | 板端 glibc | 判定 |
| --- | --- | --- | --- |
| `smart_gateway` | **GLIBC_2.28** | 2.30 | PASS（2.28 < 2.30） |
| `ota_agent` | **GLIBC_2.25** | 2.30 | PASS |
| `ota_verify` | **GLIBC_2.4** | 2.30 | PASS |

- 无 `GLIBC_PRIVATE` 引用（`grep -c` = 0），说明未误链宿主机私有符号。
- OpenSSL 符号需求为 `OPENSSL_1_1_0`，与板端 OpenSSL 1.1.1g 匹配。

### 5.6 佐证：依赖库全部来自 ARM sysroot

```text
$ .../arm-buildroot-linux-gnueabihf-gcc -print-file-name=libc.so.6
.../sysroot/lib/libc.so.6

$ find build/arm-d25 -name "*.o" | xargs file | grep -v ARM
（无输出 → 所有中间对象均为 ARM）
```

---

## 6. 本轮解决的兼容性问题：json-c 0.13.1 缺少 API

### 6.1 问题

首次交叉编译 OTA Agent 时失败：

```text
ota_state_io.c:327:17: error: implicit declaration of function
'json_tokener_get_parse_end'; did you mean 'json_tokener_parse_ex'?
```

原因：`json_tokener_get_parse_end()` 在 **json-c 0.14** 才引入，而目标 sysroot 是
**0.13.1**，宿主机是 0.18。宿主机编译通过、交叉编译失败——这正是"假定宿主机库与板端兼容"的典型陷阱。

### 6.2 为什么不能降低编译标准

用 `-Wno-error=implicit-function-declaration` 可以让编译通过，但 `json_tokener_get_parse_end`
在 0.13.1 的 **.so 里根本不存在**，链接期会报未定义符号，或（若勉强链接）在板端 dlopen 失败。
这会把问题从"编译期"推迟到"板端运行时"，是最坏的结果。

### 6.3 解决方案：显式兼容层

新增 `bsp/apps/ota_agent/ota_json_compat.h`：

```c
#if defined(JSON_C_VERSION_NUM) && (JSON_C_VERSION_NUM >= 0x000e00)
#define ota_json_parse_end(tok) json_tokener_get_parse_end(tok)   /* 0.14+ 官方 API */
#else
#define ota_json_parse_end(tok) ((size_t)((tok)->char_offset))    /* 0.13.x 公开字段 */
#endif
```

**语义等价性论证**（依据 json-c 0.13.1 源码 `json_tokener.c`）：

1. `char_offset` 在 `json_tokener_parse_ex()` 入口被置 0，每消费一个字节由 `ADVANCE_CHAR` 递增，
   语义就是"已消费字节数"，与 `json_tokener_get_parse_end()` 完全一致。
2. 解析成功返回前调用的 `json_tokener_reset_level()` **不修改 `char_offset`**，
   因此返回后该值仍是本次消费的字节数。
3. `struct json_tokener` 在 0.13.1 中是**公开定义**（非 opaque），可直接读字段。

替换了 3 处调用：`ota_state_io.c`、`ota_release.c`、`ota_upgrade.c`。

### 6.4 等价性实测验证

构造"JSON 合法但尾部有垃圾字节"的状态文件，两个 json-c 版本的行为必须一致。
这是能真正区分"检查生效"与"检查被跳过"的用例：

```text
状态文件 = {合法 JSON} + "GARBAGE"

x86  (json-c 0.18，走官方 API)        → FAIL: cannot load OTA state   (exit 1)
ARM  (json-c 0.13.1，走兼容层)        → FAIL: cannot load OTA state   (exit 1)
```

两者**均正确拒绝**，证明兼容层不是"永远返回成功"的假实现。
对照：去掉尾部垃圾的同一文件，两者均正常输出 `State : CONFIRMED`（见 7.3 节）。

---

## 7. 部署与非破坏性运行验证

### 7.1 板端部署 — BLOCKED

**未执行**。原因：本机无法访问目标板（第 10 节）。因此：

- 未向板端传输任何文件；
- 未在板端创建任何目录；
- 未在板端运行任何程序。

**这不是 PASS**。任务书要求的"部署到与正式程序隔离的测试目录"本轮无法完成。

### 7.2 替代验证：QEMU 用户态模拟（明确标注为模拟，非板端）

为在不接触板端的前提下获得"ARM 代码确实能执行"的证据，使用 `qemu-arm`
（静态解包，未安装到系统）配合目标 sysroot 运行 ARM 产物。

> **证据等级说明**：QEMU 用户态模拟能证明"ARM 指令 + 动态链接 + 库调用"链路正确，
> **不能**证明 VFP 在真实硬件上的行为、真实内核 syscall 差异、以及板端库的实际存在性。
> 因此本节结论标记为"模拟通过"，**不等同于板端 PASS**。

### 7.3 模拟运行结果

**Gateway 版本与参数解析**

```text
$ qemu-arm -L $SYSROOT build/arm-d25/smart_gateway --version
smart_gateway d25-1-arm

$ qemu-arm -L $SYSROOT build/arm-d25/smart_gateway
Specify exactly one of --serial or --no-serial
Usage: build/arm-d25/smart_gateway (--serial <device> | --no-serial) [--baud <rate>]
       --server <server_ip> --port <server_port>
```

**Gateway 隔离启动**（使用 19000 端口，避开 7000/9000）

```text
$ qemu-arm -L $SYSROOT build/arm-d25/smart_gateway \
      --no-serial --server 127.0.0.1 --port 19000

Gateway started
[BT] disabled (--no-serial)
[WIFI] listening on port 7000
[SERVER WORKER] started
[TCP] connecting to 127.0.0.1:19000...      (无服务端 → 重试，符合预期)
...
[SERVER WORKER] stopped
Gateway stopped
Valid frames    : 0
TCP connect errors : 7
```

启动、后台线程创建、优雅关闭（含各 worker join）全部正常。`EXIT=124`
是外部 `timeout` 触发的终止，不是程序崩溃。

**OTA Agent 基础命令**

```text
$ qemu-arm -L $SYSROOT build/arm-d25/ota_agent
Usage:
  ota_agent verify PACKAGE_DIR
  ota_agent status INSTALL_DIR
  ota_agent recover INSTALL_DIR
  ota_agent stage PACKAGE_DIR INSTALL_DIR
  ota_agent PACKAGE_DIR INSTALL_DIR
```

**OTA 只读包校验**（用 ARM Gateway 自身构造的真实 ARM 包）

```text
$ qemu-arm -L $SYSROOT build/arm-d25/ota_agent verify /tmp/d25-1/pkg-arm
PASS: OTA package verification successful
Application : smart_gateway
Version     : v0.1.3-d251
Size        : 199984 bytes
SHA-256     : 1beb5d2f398f31a8f786efcba79987d1eed824f91749d94b9e0d93a8c92b6303
```

SHA-256 与 `sha256sum` 独立计算值一致，说明**目标 sysroot 的 OpenSSL 1.1.1g 在 ARM 下正确工作**。

**OTA 状态只读查询**

```text
$ qemu-arm -L $SYSROOT build/arm-d25/ota_agent status /tmp/d25-1/inst
State     : CONFIRMED
Confirmed : v0.1.0
Candidate : v0.1.0
Current   : releases/v0.1.0
Release   : CONFIRMED_TARGET
```

### 7.4 负向用例（拒绝行为）

| 用例 | 期望 | 实际 | 结果 |
| --- | --- | --- | --- |
| a. SHA-256 不匹配 | 拒绝 | `FAIL: SHA-256 mismatch` | PASS |
| b. 错误架构（x86_64-linux-gnu） | 拒绝 | `FAIL: manifest metadata mismatch` | PASS |
| c. 二进制截断（199984→100000） | 拒绝 | `FAIL: binary size mismatch` | PASS |
| d. 非 ELF 内容（哈希正确） | 拒绝 | `FAIL: ELF header incomplete` | PASS |
| e. x86-64 ELF 冒充 ARM（哈希正确） | 拒绝 | `FAIL: not a supported ARM ELF` | PASS |
| f. 非法 JSON | 拒绝 | `FAIL: invalid manifest JSON` | PASS |
| g. manifest 尾部多余数据 | 见下方说明 | 接受 | **已知限制** |

用例 e 尤其重要：它证明 ARM 版的 ELF 架构检查**真的在检查**，不是走过场。

**用例 g 说明**：`ota_verify_package()` 使用 `json_object_from_file()`（一次性解析整个文件），
不校验尾部剩余字节，因此附加 `{"x":1}` 后仍被接受。该行为**在改动前就存在**，
x86 版表现完全一致（已实测对照），**不是本轮引入的回归**。
由于 manifest 的完整性由 `size` + `sha256` + ELF 头三重保护，尾部冗余字节不构成实际安全弱点，
但属于"严格性可改进项"，记入第 11 节。

### 7.5 隔离性实测

**端口冲突行为**（模拟板端正式 Gateway 已占 7000 的情形）：

```text
实例 1（x86，占用 7000）
实例 2（ARM，qemu）启动 → [WIFI] cannot listen on port 7000: Address already in use
                        → Gateway started 后立即 Gateway stopped
```

结论：**第二个实例会因端口冲突自行退出，不会抢占或干扰已运行的正式 Gateway**。
这一实测结果支持"板端可做隔离测试"的判断，但**仍需板端实机确认**。

---

## 8. 原有 x86 构建与测试回归

### 8.1 构建

```sh
make clean && make all
```

生成 `build/smart_gateway`、`build/gateway_server`、`build/mock_node`、`build/mock_wifi_node`，
全部为 `ELF 64-bit LSB pie executable, x86-64`，**零警告零错误**。

### 8.2 单元测试

```text
$ make test
test_crc16             PASS
test_frame             PASS
test_frame_parser      PASS
test_line_parser       PASS
test_message_json      PASS
test_message_queue     PASS
test_node_store        PASS
test_device_manager    PASS
test_command_manager   PASS
test_tcp_send_timeout  PASS
全部单元测试通过
```

**10/10 PASS，无回归。**

### 8.3 集成测试

```text
$ make test-tcp
server 已就绪 (端口 9000)
test_tcp_client        PASS
全部集成测试通过
```

### 8.4 OTA Agent x86 回归

```sh
cd bsp/apps/ota_agent && make clean && make all && make ota_agent_x86
```

三个 x86 产物（`ota_agent`、`ota_verify`、`ota_agent_x86`）均正常生成。

功能对照（**与 ARM 版行为一致**）：

| 用例 | x86 结果 | ARM 结果 | 一致性 |
| --- | --- | --- | --- |
| 合法 ARM 包 | PASS | PASS | ✓ |
| SHA-256 错误 | FAIL | FAIL | ✓ |
| 状态文件尾部垃圾 | FAIL | FAIL | ✓ |
| 合法状态文件 | CONFIRMED / CONFIRMED_TARGET | 同左 | ✓ |

**x86 行为与改动前完全一致**，`ota_json_compat.h` 在 json-c 0.18 下走官方 API 分支，无副作用。

---

## 9. 隔离性与无泄漏确认

### 9.1 未接触正式环境

| 检查 | 结果 |
| --- | --- |
| 是否连接过目标板 | **否**（第 10 节证据） |
| 是否停止/重启过正式 Gateway | **否** |
| 是否修改板端 OTA 状态目录 | **否** |
| 是否修改正式启动服务 | **否** |
| 是否写入板端任何文件 | **否** |

### 9.2 宿主进程与端口

测试全部完成后：

```text
$ pgrep -af "smart_gateway|qemu-arm|gateway_server"
（无输出）

$ ss -ltn | grep -E ":7000|:9000|:19000"
（无输出）
```

**进程泄漏 0，端口占用残留 0。**

所有测试均在 `/tmp/d25-1/`、`/tmp/armsmoke/` 等临时目录进行，未触碰仓库外的正式数据。

### 9.3 隔离测试目录

| 路径 | 用途 | 是否触及正式数据 |
| --- | --- | --- |
| `/tmp/d25-1/pkg-*` | 测试用 OTA 包 | 否 |
| `/tmp/d25-1/inst/` | 模拟安装目录（含 state/releases/current） | 否 |
| `/tmp/d25-1/gw/` | Gateway 运行日志 | 否 |
| `/tmp/qemu-deb/` | 解包的 qemu-arm（未安装到系统） | 否 |

---

## 10. 板端不可达证据

记录时间：2026-11-06T00:13:37+08:00

```text
$ ping -c2 -W2 192.168.10.3
2 packets transmitted, 0 received, 100% packet loss

$ (TCP 探测 192.168.10.3:22)
UNREACHABLE

$ adb devices
List of devices attached
（空）

$ ls /dev/ttyUSB* /dev/ttyACM*
无串口设备

$ ip -br -4 addr
ens33            UP             192.168.44.128/24
```

三重检查均失败：

1. **网络**：历史板端地址 `192.168.10.3` 不可达；宿主机当前在 `192.168.44.0/24` 网段，
   与板端历史网段 `192.168.10.0/24` **不同**。`192.168.44.0/24` 内扫描也未发现板端。
2. **ADB**：无设备。
3. **串口**：无 `/dev/ttyUSB*` 或 `/dev/ttyACM*` 设备节点（USB 转串口未连接或未直通到虚拟机）。

因此第 1.2 节、第 7.1 节、第 6 节（持久化目录）的板端项全部 **BLOCKED**。

---

## 11. 未解决的问题与风险

### 11.1 阻塞项（必须在 D25-2 之前解决）

| # | 问题 | 影响 | 建议动作 |
| --- | --- | --- | --- |
| B1 | 无板端访问通道（网络/ADB/串口均不可用） | 全部板端验收无法执行 | 恢复 USB 转串口直通或配置同网段 |
| B2 | 板端库存在性未核实 | 若缺 `libssl.so.1.1` / `libjson-c.so.4`，OTA Agent 无法启动 | 板端执行 `d25-1-arm-check.sh target` |
| B3 | 持久化目录未验证 | OTA 安装目录可能不可持久 | 需重启前后对比文件，不能只看路径名 |
| B4 | 板端 glibc 仅为间接证据 | 2.30 来自 D16 的链接器软链记录，非本轮实测 | 板端 `strings /lib/libc.so.6` 确认 |

### 11.2 已知限制与风险

| # | 项目 | 说明 | 风险等级 |
| --- | --- | --- | --- |
| R1 | `manifest` 尾部多余字节被接受 | 既有行为，x86/ARM 一致；有三重校验兜底 | 低 |
| R2 | `make clean` 会删除 ARM 产物 | 因 ARM 产物在 `build/` 下 | 低（可重建） |
| R3 | Gateway 固定监听 7000 | `GATEWAY_WIFI_PORT` 为编译期常量，无法命令行覆盖 | 中（影响板端隔离测试手段） |
| R4 | 未验证真实 VFP 行为 | QEMU 可模拟，但不等于硬件 | 低（Cortex-A7 支持 VFPv4） |
| R5 | 未开启 `-ffile-prefix-map` | 产物含绝对路径调试信息 | 极低 |
| R6 | 产物未 strip | 含 debug_info，便于调试但体积较大 | 极低 |

关于 R3：由于端口固定，板端做隔离测试时第二个 Gateway 实例必然因 7000 冲突退出。
这**恰好保证了不会干扰正式实例**，但也意味着"双实例并存测试"无法进行。
若 D25-2 需要，应给 `GATEWAY_WIFI_PORT` 增加运行时配置（属功能变更，不在 D25-1 范围）。

---

## 12. D25-2 是否具备启动条件

**结论：具备"启动条件"，但必须先完成板端连通性恢复。**

### 12.1 已就绪

| 条件 | 状态 |
| --- | --- |
| ARM 交叉编译链路可复现 | ✅ 一条命令完成，零警告 |
| 产物为正确 ARM ELF | ✅ EABI5 / ARMv7 / VFPv4 / hard-float |
| 解释器与板端一致 | ✅ `/lib/ld-linux-armhf.so.3` |
| glibc 符号需求满足 | ✅ 最高 2.28 < 板端 2.30 |
| 第三方库不混入宿主机版本 | ✅ `-I`/`-L` 全在 sysroot 内，无 RPATH |
| x86 流程无回归 | ✅ 10/10 单测 + 集成测试全通过 |
| 核验脚本就绪 | ✅ `bsp/tools/d25-1-arm-check.sh` |
| 测试隔离手段可行 | ✅ 端口冲突实测会自行退出 |

### 12.2 仍为 BLOCKED 的前置项

| 前置项 | 状态 |
| --- | --- |
| 板端网络/ADB/串口连通 | ❌ BLOCKED |
| 板端依赖库存在性 | ❌ BLOCKED |
| 板端持久化目录验证 | ❌ BLOCKED |
| 板端实际运行 ARM 产物 | ❌ BLOCKED |

### 12.3 建议

**可以启动 D25-2**，但 D25-2 的第一项任务必须是**恢复板端连通性并执行板端核验**，
而不是直接做升级流程。建议顺序：

1. 恢复板端连接（USB 串口直通或配置 `192.168.10.0/24`）。
2. 板端执行 `sh bsp/tools/d25-1-arm-check.sh target`，补齐第 1.2 节全部数据。
3. 按第 7.1 节补做真正部署：`scp build/arm-d25/* root@<board>:/tmp/d25-1/`。
4. 板端隔离运行 Gateway 与 OTA Agent，确认与 QEMU 结果一致。
5. 重启验证 OTA 安装目录持久性。
6. 以上全部 PASS 后，才进入 D25-2 的升级流程设计。

---

## 13. 复现命令清单

```sh
# ---- 0. 环境自检 ----
make arm-info
cd bsp/apps/ota_agent && make arm-info && cd -

# ---- 1. 构建（x86 + ARM 可共存）----
make all                    # x86
make arm                    # ARM Gateway
cd bsp/apps/ota_agent
make all                    # x86 OTA
make ota_agent_x86          # x86 集成测试版
make arm                    # ARM OTA
cd -

# ---- 2. 静态核验（不执行 ARM 代码）----
make arm-check
cd bsp/apps/ota_agent && make arm-check && cd -
sh bsp/tools/d25-1-arm-check.sh host

# ---- 3. x86 回归 ----
make test
make test-tcp

# ---- 4. ARM 运行验证（QEMU，非板端）----
SYSROOT=$HOME/Desktop/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot/arm-buildroot-linux-gnueabihf/sysroot
qemu-arm -L $SYSROOT build/arm-d25/smart_gateway --version
qemu-arm -L $SYSROOT build/arm-d25/ota_agent verify <package_dir>
qemu-arm -L $SYSROOT build/arm-d25/ota_agent status <install_dir>

# ---- 5. 板端核验（恢复连接后执行）----
scp bsp/tools/d25-1-arm-check.sh root@<board>:/tmp/
ssh root@<board> 'sh /tmp/d25-1-arm-check.sh target'
```

---

## 14. 本轮改动清单

| 文件 | 改动 |
| --- | --- |
| `Makefile` | 新增 `arm` / `arm-check` / `arm-info` / `clean-arm` 目标及 ARM 工具链配置 |
| `bsp/apps/ota_agent/Makefile` | 新增 `arm` / `arm-ota_agent` / `arm-ota_verify` / `arm-check` / `arm-info` / `clean-arm`；修正文件头注释（原件把 x86 产物误标为 ARM） |
| `bsp/apps/ota_agent/ota_json_compat.h` | **新增** json-c 版本兼容层（0.13.x / 0.14+） |
| `bsp/apps/ota_agent/ota_state_io.c` | 一处 `json_tokener_get_parse_end` → `ota_json_parse_end` |
| `bsp/apps/ota_agent/ota_release.c` | 同上 |
| `bsp/apps/ota_agent/ota_upgrade.c` | 同上 |
| `bsp/tools/d25-1-arm-check.sh` | **新增** 宿主静态核验 + 板端运行时核验脚本 |
| `bsp/docs/D25-1-arm-build-deployment-validation.md` | **新增** 本文档 |

**未改动**：`gateway/`、`common/`、`server/` 任何源码；`tests/`；x86 构建参数；任何板端配置。
