
# ============================================================
# SmartGateway-BSP OTA Agent Makefile
#
# Targets:
#   ota_verify     - ARM OTA package verification CLI
#   ota_agent      - ARM-target package OTA Agent (host tool)
#   ota_agent_x86  - x86-64 OTA integration test Agent
#
# Note:
#   All binaries are compiled by the Ubuntu host GCC.
#   The target architecture macro controls the ELF and
#   manifest architecture validation rules.
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
	ota_upgrade_run.h


# ============================================================
# Build targets
# ============================================================

.PHONY: all clean

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
# Clean
# ============================================================

clean:
	rm -f ota_verify ota_agent ota_agent_x86
