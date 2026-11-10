
# ============================================================
# SmartGateway-BSP OTA Agent Makefile
#
# 宿主机（x86-64）目标 —— 保持既有流程不变
#   ota_verify     - x86-64 包校验 CLI
#   ota_agent      - x86-64 OTA Agent（ARM 目标架构校验规则）
#   ota_agent_x86  - x86-64 OTA 集成测试 Agent（-DOTA_TARGET_X86_64）
#
# ARM（i.MX6ULL）目标 —— D25-1 新增，真实交叉编译
#   arm            - 构建全部 ARM 产物到 build/arm-d25/
#   arm-ota_agent  - ARM OTA Agent
#   arm-ota_verify - ARM 包校验 CLI
#   arm-check      - 只读检查 ARM ELF 架构/解释器/依赖
#
# 关键约束：
#   1. ARM 目标必须使用目标 sysroot 的头文件与库，
#      不得把宿主机 /usr/include、/usr/lib 的 OpenSSL/json-c 混入。
#   2. ARM 产物输出到独立目录，不覆盖原有 x86 二进制。
#   3. 原有 x86 目标行为与本轮之前保持一致。
# ============================================================

# ============================================================
# 交叉编译工具链配置
#
# 选择依据（D25-1 核验结论）：
#   目标板 rootfs 为 Buildroot 2020.02，glibc 2.30，
#   动态链接器 /lib/ld-linux-armhf.so.3 -> ld-2.30.so，
#   且 rootfs 自带 OpenSSL 1.1.1g 与 json-c 0.13.1。
#   宿主机上的 Linaro GCC 6.2.1 自带 sysroot 为 glibc 2.23，
#   而且完全不含 OpenSSL/json-c，直接使用会与板端
#   ABI 与库版本不匹配。
#   因此选择与板端同源的 Buildroot SDK 工具链。
#
# 可通过命令行覆盖，例如：
#   make arm ARM_SDK_DIR=/opt/sdk
# ============================================================

ARM_SDK_DIR ?= $(HOME)/Desktop/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot
ARM_CROSS   ?= $(ARM_SDK_DIR)/bin/arm-buildroot-linux-gnueabihf-
ARM_CC      := $(ARM_CROSS)gcc
ARM_READELF := $(ARM_CROSS)readelf

# 目标 sysroot：显式指定，避免默认搜索路径与宿主机头文件混淆
ARM_SYSROOT ?= $(ARM_SDK_DIR)/arm-buildroot-linux-gnueabihf/sysroot

# 目标板 CPU：i.MX6ULL 为单核 Cortex-A7，ARMv7-A + VFPv4，hard-float
ARM_ARCH_FLAGS := -mcpu=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4 -marm

# 独立输出目录，不覆盖宿主机 build/ 下的 x86 产物
ARM_BUILD_DIR ?= ../../../build/arm-d25

# ============================================================
# 宿主机（x86-64）配置
# ============================================================

CC := gcc

CFLAGS := -std=c11 -O0 -g -Wall -Wextra -Werror

PKG_CFLAGS := $(shell pkg-config --cflags openssl json-c)
PKG_LIBS := $(shell pkg-config --libs openssl json-c)


# ============================================================
# OTA shared source files
# ============================================================

OTA_SRCS := \
	main.c \
	ota_verify.c \
	ota_state.c \
	ota_state_io.c \
	ota_recover.c \
	ota_lock.c \
	ota_release.c \
	ota_upgrade.c \
	ota_process.c \
	ota_health.c \
	ota_supervisor.c \
	ota_oplog.c \
	ota_upgrade_run.c


# ============================================================
# OTA shared header files
# ============================================================

OTA_HEADERS := \
	ota_verify.h \
	ota_state.h \
	ota_state_io.h \
	ota_recover.h \
	ota_lock.h \
	ota_release.h \
	ota_upgrade.h \
	ota_process.h \
	ota_health.h \
	ota_supervisor.h \
	ota_oplog.h \
	ota_upgrade_run.h \
	ota_json_compat.h


# ============================================================
# Build targets
# ============================================================

.PHONY: all clean clean-arm \
	arm arm-ota_agent arm-ota_verify arm-check arm-info

all: ota_verify ota_agent


# ============================================================
# 1. Standalone OTA verifier
#    Default: ARMv7 package validation
# ============================================================

ota_verify: ota_verify.c ota_verify_cli.c ota_verify.h
	$(CC) $(CFLAGS) $(PKG_CFLAGS) \
		ota_verify.c ota_verify_cli.c \
		-o $@ $(PKG_LIBS)


# ============================================================
# 2. Default OTA Agent
#    ARMv7 package validation configuration
# ============================================================

ota_agent: $(OTA_SRCS) $(OTA_HEADERS)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) \
		$(OTA_SRCS) \
		-o $@ $(PKG_LIBS)


# ============================================================
# 3. x86-64 OTA Agent
#    Ubuntu integration testing only
# ============================================================

ota_agent_x86: $(OTA_SRCS) $(OTA_HEADERS)
	$(CC) $(CFLAGS) -DOTA_TARGET_X86_64 \
		$(PKG_CFLAGS) \
		$(OTA_SRCS) \
		-o $@ $(PKG_LIBS)


# ============================================================
# 4. ARM (i.MX6ULL) targets
#
# 只使用目标 sysroot 的头文件与库：
#   - --sysroot 让 <> 头文件与 -l 库全部解析到目标 sysroot
#   - 使用 SDK 自带 pkg-config 包装脚本，其 PKG_CONFIG_SYSROOT_DIR /
#     PKG_CONFIG_LIBDIR 指向目标 sysroot，不会读到宿主机 .pc 文件
# ============================================================

# SDK 自带 pkg-config 包装：只查询目标 sysroot 内的 .pc
ARM_PKG_CONFIG ?= $(ARM_SDK_DIR)/bin/pkg-config

# 显式锁定 pkg-config 搜索路径，即使调用者环境里设置了
# 宿主机变量也不会把 x86 的 OpenSSL/json-c 带进来。
ARM_PKG_ENV := \
	PKG_CONFIG_LIBDIR=$(ARM_SYSROOT)/usr/lib/pkgconfig:$(ARM_SYSROOT)/usr/share/pkgconfig \
	PKG_CONFIG_SYSROOT_DIR=$(ARM_SYSROOT)

ARM_PKG_CFLAGS := $(shell $(ARM_PKG_ENV) $(ARM_PKG_CONFIG) --cflags openssl json-c)
ARM_PKG_LIBS   := $(shell $(ARM_PKG_ENV) $(ARM_PKG_CONFIG) --libs openssl json-c)

ARM_CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror -pthread \
	$(ARM_ARCH_FLAGS) --sysroot=$(ARM_SYSROOT)

ARM_LDFLAGS := -pthread --sysroot=$(ARM_SYSROOT)

arm: arm-ota_verify arm-ota_agent

arm-ota_verify: $(ARM_BUILD_DIR)/ota_verify

arm-ota_agent: $(ARM_BUILD_DIR)/ota_agent

$(ARM_BUILD_DIR)/ota_verify: ota_verify.c ota_verify_cli.c ota_verify.h ota_json_compat.h
	@mkdir -p $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_CFLAGS) $(ARM_PKG_CFLAGS) \
		ota_verify.c ota_verify_cli.c \
		-o $@ $(ARM_PKG_LIBS) $(ARM_LDFLAGS)

# ARM OTA Agent：显式定义 OTA_TARGET_ARM_V7，便于区分构建形态；
# 未定义 OTA_TARGET_X86_64，因此仍保持 ARM 目标校验规则。
$(ARM_BUILD_DIR)/ota_agent: $(OTA_SRCS) $(OTA_HEADERS)
	@mkdir -p $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_CFLAGS) -DOTA_TARGET_ARM_V7 \
		$(ARM_PKG_CFLAGS) \
		$(OTA_SRCS) \
		-o $@ $(ARM_PKG_LIBS) $(ARM_LDFLAGS)


# ============================================================
# ARM 产物自检：架构、解释器、依赖库、ABI
#
# readelf/file 只读取文件，不执行 ARM 代码，因此没有 QEMU
# 或目标板时也能给出静态证据。
# ============================================================

arm-check: arm
	@echo "== ELF 架构 =="
	@file $(ARM_BUILD_DIR)/ota_agent $(ARM_BUILD_DIR)/ota_verify
	@echo "== 动态解释器 =="
	@$(ARM_READELF) -l $(ARM_BUILD_DIR)/ota_agent | grep -i interpreter
	@echo "== 依赖库 =="
	@$(ARM_READELF) -d $(ARM_BUILD_DIR)/ota_agent | grep NEEDED
	@echo "== ABI 属性 =="
	@$(ARM_READELF) -A $(ARM_BUILD_DIR)/ota_agent | grep -E 'Tag_CPU_arch|Tag_FP_arch|Tag_ABI_VFP_args'

arm-info:
	@echo "ARM_CC         = $(ARM_CC)"
	@echo "ARM_SYSROOT    = $(ARM_SYSROOT)"
	@echo "ARM_BUILD_DIR  = $(ARM_BUILD_DIR)"
	@echo "ARM_CFLAGS     = $(ARM_CFLAGS)"
	@echo "ARM_PKG_CFLAGS = $(ARM_PKG_CFLAGS)"
	@echo "ARM_PKG_LIBS   = $(ARM_PKG_LIBS)"


# ============================================================
# 5. D25-2 S1：Supervisor 单元测试
#
# 只测 ota_supervisor.c，不链接其他 OTA 模块，
# 因此不会触及 D23 已验证的升级逻辑。
# ============================================================

.PHONY: test-supervisor test-supervisor-s2 test-supervisor-arm

SUP_TEST_BIN  := $(ARM_BUILD_DIR)/ota_supervisor_test
S2_TEST_BIN   := $(ARM_BUILD_DIR)/ota_supervisor_s2_test
E2E_BIN       := $(ARM_BUILD_DIR)/ota_supervisor_e2e
HOST_SUP_BIN  := /tmp/ota_supervisor_test_host
HOST_S2_BIN   := /tmp/ota_supervisor_s2_test_host

# 测试超时（秒）
#
# 程序内部的 elapsed 断言只能在返回后生效，
# 若测试自身挂起（例如 fork 后子进程未退出、
# 父进程无界 waitpid），断言根本执行不到。
# 必须由外部 timeout 兜底。
TEST_TIMEOUT ?= 120

# S1 宿主机测试
#
# ota_supervisor.c 现在包含 S2 的实现，而 S2 调用 ota_process_*，
# 因此即使是 S1 测试也必须链接 ota_process.c。
# ota_process.c 在 -Werror 下有既存的 unused-result 告警，
# 该文件属 D23 已验证代码，本阶段不修改，故单独放宽该告警。
test-supervisor:
	$(CC) $(CFLAGS) -Wno-unused-result \
		ota_supervisor.c ota_process.c \
		ota_supervisor_test.c \
		-o $(HOST_SUP_BIN)
	@timeout $(TEST_TIMEOUT) $(HOST_SUP_BIN) || \
		{ rc=$$?; [ $$rc -eq 124 ] && echo "FAIL: 测试超时 ($(TEST_TIMEOUT)s)"; exit $$rc; }

# S2 宿主机测试
test-supervisor-s2:
	$(CC) $(CFLAGS) -Wno-unused-result \
		ota_supervisor.c ota_process.c \
		ota_supervisor_s2_test.c \
		-o $(HOST_S2_BIN)
	@timeout $(TEST_TIMEOUT) $(HOST_S2_BIN) || \
		{ rc=$$?; [ $$rc -eq 124 ] && echo "FAIL: 测试超时 ($(TEST_TIMEOUT)s)"; exit $$rc; }

# ARM 版本（需要板端执行，此处只构建）
test-supervisor-arm:
	@mkdir -p $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_CFLAGS) -Wno-unused-result \
		-DOTA_TARGET_ARM_V7 \
		ota_supervisor.c ota_process.c \
		ota_supervisor_test.c \
		-o $(SUP_TEST_BIN)
	$(ARM_CC) $(ARM_CFLAGS) -Wno-unused-result \
		-DOTA_TARGET_ARM_V7 \
		ota_supervisor.c ota_process.c \
		ota_supervisor_s2_test.c \
		-o $(S2_TEST_BIN)
	$(ARM_CC) $(ARM_CFLAGS) -Wno-unused-result \
		-DOTA_TARGET_ARM_V7 \
		ota_supervisor.c ota_process.c \
		ota_supervisor_e2e.c \
		-o $(E2E_BIN)
	@echo "已构建:"
	@echo "  $(SUP_TEST_BIN)"
	@echo "  $(S2_TEST_BIN)"
	@echo "  $(E2E_BIN)"
	@echo ""
	@echo "板端端到端验证（需要真实 Gateway）："
	@echo "  adb push $(E2E_BIN) /tmp/e2e-bin"
	@echo "  adb push build/arm-d25/smart_gateway /tmp/gw"
	@echo "  adb shell 'mkdir -p /tmp/e2e-inst && /tmp/e2e-bin /tmp/e2e-inst /tmp/gw'"


# ============================================================
# Clean
# ============================================================

clean:
	rm -f ota_verify ota_agent ota_agent_x86

clean-arm:
	rm -rf $(ARM_BUILD_DIR)
