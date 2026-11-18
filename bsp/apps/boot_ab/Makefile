CC := gcc
CFLAGS := -std=gnu11 -O0 -g -Wall -Wextra -Werror

COMMON_DIR := ../../../common
OTA_DIR := ../ota_agent
INC_FLAGS := -I. -I$(COMMON_DIR)

COMMON_SRCS := ab_state.c ab_health.c fw_env_backend.c $(COMMON_DIR)/service_ipc.c
COMMON_HDRS := ab_state.h ab_health.h fw_env_backend.h $(COMMON_DIR)/service_ipc.h
OTA_COMMON_SRCS := $(OTA_DIR)/ota_state_io.c $(OTA_DIR)/ota_state.c $(OTA_DIR)/ota_oplog.c
R3C_SRCS := r3c_data.c r3c_policy.c r3c_binding.c r3c_bundle.c r3c_txn.c r3c_coordinator.c health_devices.c health_network.c health_gateway.c health_supervisor.c system_health.c $(OTA_COMMON_SRCS)
R3C_HDRS := r3c_data.h r3c_policy.h r3c_binding.h r3c_bundle.h r3c_txn.h r3c_coordinator.h health_devices.h health_network.h health_gateway.h health_supervisor.h system_health.h
R3C_INC := $(INC_FLAGS) -I$(OTA_DIR)
R3C_LIBS := -ljson-c -lcrypto

ARM_SDK_DIR ?= $(HOME)/Desktop/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot
ARM_CROSS   ?= $(ARM_SDK_DIR)/bin/arm-buildroot-linux-gnueabihf-
ARM_CC      := $(ARM_CROSS)gcc
ARM_SYSROOT ?= $(ARM_SDK_DIR)/arm-buildroot-linux-gnueabihf/sysroot
ARM_ARCH_FLAGS := -mcpu=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4 -marm
ARM_BUILD_DIR ?= ../../../build/arm-ab

TEST_TIMEOUT ?= 60

.PHONY: all clean clean-arm arm arm-check test

all: sgw_bootctl sgw_watchdog_guard sgw_system_health

sgw_bootctl: sgw_bootctl.c $(COMMON_SRCS) $(COMMON_HDRS) $(R3C_SRCS) $(R3C_HDRS)
	$(CC) $(CFLAGS) $(R3C_INC) sgw_bootctl.c $(COMMON_SRCS) $(R3C_SRCS) $(R3C_LIBS) -o $@

sgw_watchdog_guard: sgw_watchdog_guard.c $(COMMON_SRCS) $(COMMON_HDRS) $(R3C_SRCS) $(R3C_HDRS)
	$(CC) $(CFLAGS) $(R3C_INC) sgw_watchdog_guard.c $(COMMON_SRCS) $(R3C_SRCS) $(R3C_LIBS) -o $@

sgw_system_health: sgw_system_health.c health_devices.c health_devices.h health_network.c health_network.h health_gateway.c health_gateway.h health_supervisor.c health_supervisor.h system_health.c system_health.h $(COMMON_SRCS) $(COMMON_HDRS)
	$(CC) $(CFLAGS) $(INC_FLAGS) sgw_system_health.c health_devices.c health_network.c health_gateway.c health_supervisor.c system_health.c $(COMMON_SRCS) -o $@

arm:
	@mkdir -p $(ARM_BUILD_DIR)
	$(ARM_CC) $(CFLAGS) \
		$(ARM_ARCH_FLAGS) \
		--sysroot=$(ARM_SYSROOT) \
		$(R3C_INC) \
		sgw_bootctl.c $(COMMON_SRCS) $(R3C_SRCS) $(R3C_LIBS) \
		-o $(ARM_BUILD_DIR)/sgw_bootctl
	$(ARM_CC) $(CFLAGS) \
		$(ARM_ARCH_FLAGS) \
		--sysroot=$(ARM_SYSROOT) \
		$(R3C_INC) \
		sgw_watchdog_guard.c $(COMMON_SRCS) $(R3C_SRCS) $(R3C_LIBS) \
		-o $(ARM_BUILD_DIR)/sgw_watchdog_guard
	$(ARM_CC) $(CFLAGS) $(ARM_ARCH_FLAGS) --sysroot=$(ARM_SYSROOT) \
		$(INC_FLAGS) sgw_system_health.c health_devices.c health_network.c health_gateway.c health_supervisor.c system_health.c $(COMMON_SRCS) \
		-o $(ARM_BUILD_DIR)/sgw_system_health
	@ls -lh $(ARM_BUILD_DIR)/sgw_bootctl $(ARM_BUILD_DIR)/sgw_watchdog_guard

arm-check: arm
	@for bin in sgw_bootctl sgw_watchdog_guard sgw_system_health; do \
		echo "== $$bin =="; \
		$(ARM_CROSS)readelf -h $(ARM_BUILD_DIR)/$$bin | grep -E "Class|Machine|Type"; \
		$(ARM_CROSS)readelf -d $(ARM_BUILD_DIR)/$$bin | grep NEEDED || true; \
		$(ARM_CROSS)readelf --version-info $(ARM_BUILD_DIR)/$$bin | grep -o "GLIBC_[0-9.]*" | sort -Vu | tail -1 || true; \
	done

test: ab_state_test fw_env_backend_test ab_health_test system_health_test health_devices_test health_network_test r3c_binding_test r3c_data_exclusion_test r3c_confirm_harness r3c_probe_cli r3c_bundle_cli supervisor_probe_cli gateway_probe_cli sgw_bootctl sgw_watchdog_guard sgw_system_health
	@timeout $(TEST_TIMEOUT) ./ab_state_test
	@timeout $(TEST_TIMEOUT) ./fw_env_backend_test
	@timeout $(TEST_TIMEOUT) ./ab_health_test
	@timeout $(TEST_TIMEOUT) ./system_health_test
	@timeout $(TEST_TIMEOUT) ./health_devices_test
	@timeout $(TEST_TIMEOUT) ./health_network_test
	@timeout $(TEST_TIMEOUT) ./r3c_binding_test
	@timeout $(TEST_TIMEOUT) ./r3c_data_exclusion_test
	@cd ../../.. && timeout $(TEST_TIMEOUT) python3 -m unittest -v tests.test_g2r3c_data
	@cd ../../.. && timeout $(TEST_TIMEOUT) python3 -m unittest -v tests.test_g2r3c_signed_bundle

ab_state_test: tests/ab_state_test.c ab_state.c ab_state.h
	$(CC) $(CFLAGS) -I. tests/ab_state_test.c ab_state.c -o $@

fw_env_backend_test: tests/fw_env_backend_test.c fw_env_backend.c fw_env_backend.h ab_state.c ab_state.h
	$(CC) $(CFLAGS) -I. tests/fw_env_backend_test.c fw_env_backend.c ab_state.c -o $@

ab_health_test: tests/ab_health_test.c ab_health.c ab_health.h ab_state.c ab_state.h $(COMMON_DIR)/service_ipc.c $(COMMON_DIR)/service_ipc.h
	$(CC) $(CFLAGS) $(INC_FLAGS) tests/ab_health_test.c ab_health.c ab_state.c $(COMMON_DIR)/service_ipc.c -o $@

system_health_test: tests/system_health_test.c system_health.c system_health.h ab_state.c ab_state.h
	$(CC) $(CFLAGS) $(INC_FLAGS) tests/system_health_test.c system_health.c ab_state.c -o $@

health_devices_test: tests/health_devices_test.c health_devices.c health_devices.h system_health.h
	$(CC) $(CFLAGS) $(INC_FLAGS) tests/health_devices_test.c health_devices.c -o $@

health_network_test: tests/health_network_test.c health_network.c health_network.h health_supervisor.c health_supervisor.h $(COMMON_DIR)/service_ipc.c $(COMMON_DIR)/service_ipc.h
	$(CC) $(CFLAGS) $(INC_FLAGS) tests/health_network_test.c health_network.c health_supervisor.c $(COMMON_DIR)/service_ipc.c -o $@

supervisor_probe_cli: tests/supervisor_probe_cli.c health_supervisor.c health_supervisor.h $(COMMON_DIR)/service_ipc.c $(COMMON_DIR)/service_ipc.h
	$(CC) $(CFLAGS) $(INC_FLAGS) tests/supervisor_probe_cli.c health_supervisor.c $(COMMON_DIR)/service_ipc.c -o $@

gateway_probe_cli: tests/gateway_probe_cli.c health_gateway.c health_gateway.h health_supervisor.c health_supervisor.h $(COMMON_DIR)/service_ipc.c $(COMMON_DIR)/service_ipc.h
	$(CC) $(CFLAGS) $(INC_FLAGS) tests/gateway_probe_cli.c health_gateway.c health_supervisor.c $(COMMON_DIR)/service_ipc.c -o $@

r3c_binding_test: tests/r3c_binding_test.c r3c_binding.c r3c_binding.h
	$(CC) $(CFLAGS) -I. tests/r3c_binding_test.c r3c_binding.c -o $@

r3c_probe_cli: tests/r3c_probe_cli.c r3c_data.c r3c_data.h $(OTA_DIR)/ota_state_io.c $(OTA_DIR)/ota_state.c $(OTA_DIR)/ota_oplog.c
	$(CC) $(CFLAGS) -I. -I$(OTA_DIR) tests/r3c_probe_cli.c r3c_data.c $(OTA_DIR)/ota_state_io.c $(OTA_DIR)/ota_state.c $(OTA_DIR)/ota_oplog.c -ljson-c -o $@

r3c_bundle_cli: tests/r3c_bundle_cli.c r3c_bundle.c r3c_bundle.h
	$(CC) $(CFLAGS) -I. tests/r3c_bundle_cli.c r3c_bundle.c -lcrypto -o $@

r3c_data_exclusion_test: tests/r3c_data_exclusion_test.c r3c_data.c r3c_data.h r3c_policy.c r3c_policy.h $(OTA_COMMON_SRCS)
	$(CC) $(CFLAGS) $(R3C_INC) tests/r3c_data_exclusion_test.c r3c_data.c r3c_policy.c $(OTA_COMMON_SRCS) -ljson-c -o $@

r3c_confirm_harness: tests/r3c_confirm_harness.c $(COMMON_SRCS) $(COMMON_HDRS) $(R3C_SRCS) $(R3C_HDRS)
	$(CC) $(CFLAGS) $(R3C_INC) tests/r3c_confirm_harness.c $(COMMON_SRCS) $(R3C_SRCS) $(R3C_LIBS) -o $@

# Test-only ARM artifact; no production component or confirmation integration.
arm-r3c-check:
	@mkdir -p $(ARM_BUILD_DIR)
	$(ARM_CC) $(CFLAGS) $(ARM_ARCH_FLAGS) --sysroot=$(ARM_SYSROOT) -I. -I$(OTA_DIR) \
		tests/r3c_probe_cli.c r3c_data.c $(OTA_DIR)/ota_state_io.c $(OTA_DIR)/ota_state.c $(OTA_DIR)/ota_oplog.c -ljson-c -o $(ARM_BUILD_DIR)/r3c-probe-test
	$(ARM_CC) $(CFLAGS) $(ARM_ARCH_FLAGS) --sysroot=$(ARM_SYSROOT) -I. \
		tests/r3c_binding_test.c r3c_binding.c -o $(ARM_BUILD_DIR)/r3c-binding-test
	$(ARM_CC) $(CFLAGS) $(ARM_ARCH_FLAGS) --sysroot=$(ARM_SYSROOT) -I. \
		tests/r3c_bundle_cli.c r3c_bundle.c -lcrypto -o $(ARM_BUILD_DIR)/r3c-bundle-test
	@for bin in r3c-probe-test r3c-binding-test r3c-bundle-test; do \
		echo "== $$bin =="; \
		$(ARM_CROSS)readelf -h $(ARM_BUILD_DIR)/$$bin | grep -E "Class|Machine|Type"; \
		$(ARM_CROSS)readelf -d $(ARM_BUILD_DIR)/$$bin | grep NEEDED || true; \
		$(ARM_CROSS)readelf --version-info $(ARM_BUILD_DIR)/$$bin | grep -o "GLIBC_[0-9.]*" | sort -Vu | tail -1 || true; \
	done

clean:
	rm -f sgw_bootctl sgw_watchdog_guard sgw_system_health supervisor_probe_cli gateway_probe_cli ab_state_test fw_env_backend_test ab_health_test system_health_test health_devices_test health_network_test r3c_binding_test r3c_data_exclusion_test r3c_confirm_harness r3c_probe_cli r3c_bundle_cli

clean-arm:
	rm -rf $(ARM_BUILD_DIR)
