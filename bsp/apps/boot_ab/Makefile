CC := gcc
CFLAGS := -std=gnu11 -O0 -g -Wall -Wextra -Werror

SRCS := sgw_bootctl.c ab_state.c fw_env_backend.c
HDRS := ab_state.h fw_env_backend.h

ARM_SDK_DIR ?= $(HOME)/Desktop/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot
ARM_CROSS   ?= $(ARM_SDK_DIR)/bin/arm-buildroot-linux-gnueabihf-
ARM_CC      := $(ARM_CROSS)gcc
ARM_SYSROOT ?= $(ARM_SDK_DIR)/arm-buildroot-linux-gnueabihf/sysroot
ARM_ARCH_FLAGS := -mcpu=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4 -marm
ARM_BUILD_DIR ?= ../../../build/arm-ab

TEST_TIMEOUT ?= 60

.PHONY: all clean clean-arm arm arm-check test

all: sgw_bootctl

sgw_bootctl: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -I. $(SRCS) -o $@

arm:
	@mkdir -p $(ARM_BUILD_DIR)
	$(ARM_CC) $(CFLAGS) \
		$(ARM_ARCH_FLAGS) \
		--sysroot=$(ARM_SYSROOT) \
		-I. \
		$(SRCS) \
		-o $(ARM_BUILD_DIR)/sgw_bootctl
	@ls -lh $(ARM_BUILD_DIR)/sgw_bootctl

arm-check: arm
	@$(ARM_CROSS)readelf -h $(ARM_BUILD_DIR)/sgw_bootctl \
		| grep -E "Class|Machine|Type"
	@$(ARM_CROSS)readelf -d $(ARM_BUILD_DIR)/sgw_bootctl \
		| grep NEEDED || true
	@$(ARM_CROSS)readelf --version-info $(ARM_BUILD_DIR)/sgw_bootctl \
		| grep -o "GLIBC_[0-9.]*" | sort -Vu | tail -1 || true

test: ab_state_test fw_env_backend_test sgw_bootctl
	@timeout $(TEST_TIMEOUT) ./ab_state_test
	@timeout $(TEST_TIMEOUT) ./fw_env_backend_test

ab_state_test: tests/ab_state_test.c ab_state.c ab_state.h
	$(CC) $(CFLAGS) -I. tests/ab_state_test.c ab_state.c -o $@

fw_env_backend_test: tests/fw_env_backend_test.c fw_env_backend.c fw_env_backend.h ab_state.c ab_state.h
	$(CC) $(CFLAGS) -I. tests/fw_env_backend_test.c fw_env_backend.c ab_state.c -o $@

clean:
	rm -f sgw_bootctl ab_state_test fw_env_backend_test

clean-arm:
	rm -rf $(ARM_BUILD_DIR)
