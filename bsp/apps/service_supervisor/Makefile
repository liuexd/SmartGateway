# ============================================================
# Service Supervisor (S5)
#
# 产出：
#   service_supervisor        x86-64（本地测试）
#   板端产物输出到 build/arm-s5/
#
# 它解决 S3/S4 遗留的两件事：
#   L3  OTA 前置校验依赖 pidfile 存在
#   L6  停止外部进程的 TOCTOU 窗口（pidfd 在 4.9.88 不可用）
#
# 做法是把 Gateway 的所有权交给常驻 supervisor：
#   - 它是 Gateway 的父进程，waitpid 总是有效
#   - pidfile 由它维护，永远与真实状态一致
#   - 停止动作作用于自己的子进程，pid 不可能被复用
# ============================================================

CC := gcc

CFLAGS := -std=gnu11 -O0 -g -Wall -Wextra -Werror
LDFLAGS :=

COMMON_DIR := ../../../common
SRCS := main.c service_supervisor.c \
        $(COMMON_DIR)/service_ipc.c
HDRS := service_supervisor.h \
        $(COMMON_DIR)/service_ipc.h

# 与 OTA Agent 相同的工具链，避免出现两套 ABI
ARM_SDK_DIR ?= $(HOME)/Desktop/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot
ARM_CROSS   ?= $(ARM_SDK_DIR)/bin/arm-buildroot-linux-gnueabihf-
ARM_CC      := $(ARM_CROSS)gcc
ARM_SYSROOT ?= $(ARM_SDK_DIR)/arm-buildroot-linux-gnueabihf/sysroot
ARM_ARCH_FLAGS := -mcpu=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4 -marm
ARM_BUILD_DIR ?= ../../../build/arm-s5

# 测试超时：防止某个用例挂死后拖住整个 CI
TEST_TIMEOUT ?= 120

.PHONY: all clean arm arm-check test test-ipc

all: service_supervisor

service_supervisor: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -I$(COMMON_DIR) $(SRCS) -o $@ $(LDFLAGS)

# ------------------------------------------------------------
# ARM 交叉编译
# ------------------------------------------------------------
arm:
	@mkdir -p $(ARM_BUILD_DIR)
	$(ARM_CC) $(CFLAGS) -pthread \
		$(ARM_ARCH_FLAGS) \
		--sysroot=$(ARM_SYSROOT) \
		-DOTA_TARGET_ARM_V7 \
		-I$(COMMON_DIR) \
		$(SRCS) \
		-o $(ARM_BUILD_DIR)/service_supervisor
	@echo ""
	@echo "ARM 产物: $(ARM_BUILD_DIR)/service_supervisor"
	@ls -l $(ARM_BUILD_DIR)/service_supervisor

arm-check:
	@test -f $(ARM_BUILD_DIR)/service_supervisor \
		|| { echo "缺少 ARM 产物，请先 make arm"; exit 1; }
	@$(ARM_CROSS)readelf -h $(ARM_BUILD_DIR)/service_supervisor \
		| grep -E "Class|Machine|Type"
	@echo ""
	@$(ARM_CROSS)readelf -d $(ARM_BUILD_DIR)/service_supervisor \
		| grep NEEDED || true
	@echo ""
	@echo "最高 GLIBC 版本需求:"
	@$(ARM_CROSS)readelf --version-info $(ARM_BUILD_DIR)/service_supervisor \
		| grep -o "GLIBC_[0-9.]*" | sort -Vu | tail -1 || true

# ------------------------------------------------------------
# 测试
# ------------------------------------------------------------
test: test-ipc
	@echo ""
	@echo "== supervisor 单元测试 =="
	@timeout $(TEST_TIMEOUT) ./service_supervisor_test || \
		{ echo "FAIL: service_supervisor_test"; exit 1; }

test-ipc: service_supervisor_test
	@echo "== IPC 协议测试 =="
	@timeout $(TEST_TIMEOUT) ./service_ipc_test || \
		{ echo "FAIL: service_ipc_test"; exit 1; }

service_ipc_test: $(COMMON_DIR)/service_ipc_test.c \
                  $(COMMON_DIR)/service_ipc.c \
                  $(COMMON_DIR)/service_ipc.h
	$(CC) $(CFLAGS) -I$(COMMON_DIR) \
		$(COMMON_DIR)/service_ipc_test.c \
		$(COMMON_DIR)/service_ipc.c \
		-o $@

service_supervisor_test: tests/service_supervisor_test.c \
                         service_supervisor.c \
                         service_supervisor.h \
                         $(COMMON_DIR)/service_ipc.c \
                         $(COMMON_DIR)/service_ipc.h
	$(CC) $(CFLAGS) -I$(COMMON_DIR) -I. \
		tests/service_supervisor_test.c \
		service_supervisor.c \
		$(COMMON_DIR)/service_ipc.c \
		-o $@

clean:
	rm -f service_supervisor service_ipc_test service_supervisor_test

clean-arm:
	rm -rf $(ARM_BUILD_DIR)
