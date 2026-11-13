#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

UBOOT_SRC="${UBOOT_SRC_DIR:-$HOME/Desktop/Uboot-2017.03}"
PATCH_FILE="$SCRIPT_DIR/patches/0001-mx6ullevk-ab-bootcount-redund-env-watchdog.patch"
OUT_ROOT="${UBOOT_AB_BUILD_DIR:-$REPO_DIR/build/uboot-ab}"
WORK_SRC="$OUT_ROOT/src"
ARM_OUT="$OUT_ROOT/arm"
HOST_OUT="$OUT_ROOT/host"

LINARO_CROSS="${LINARO_CROSS:-$HOME/Desktop/ToolChain/gcc-linaro-6.2.1-2016.11-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-}"
BR_SDK_DIR="${ARM_SDK_DIR:-$HOME/Desktop/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot}"
BR_CROSS="${BR_CROSS:-$BR_SDK_DIR/bin/arm-buildroot-linux-gnueabihf-}"
BR_SYSROOT="${BR_SYSROOT:-$BR_SDK_DIR/arm-buildroot-linux-gnueabihf/sysroot}"

JOBS="${JOBS:-$(nproc)}"

if [ ! -d "$UBOOT_SRC/.git" ]; then
    echo "ERROR: U-Boot git repo not found at $UBOOT_SRC" >&2
    exit 1
fi
if [ ! -f "$PATCH_FILE" ]; then
    echo "ERROR: Patch file not found at $PATCH_FILE" >&2
    exit 1
fi
if [ ! -x "${LINARO_CROSS}gcc" ]; then
    echo "ERROR: Linaro toolchain not found at ${LINARO_CROSS}gcc" >&2
    exit 1
fi

echo "=== Preparing isolated U-Boot 2017.03 source in $WORK_SRC ==="
rm -rf "$WORK_SRC" "$ARM_OUT" "$HOST_OUT"
mkdir -p "$WORK_SRC" "$ARM_OUT" "$HOST_OUT"

git -C "$UBOOT_SRC" archive HEAD | tar -x -C "$WORK_SRC"
git -C "$WORK_SRC" init -q
git -C "$WORK_SRC" config user.email "build@smartgateway.local"
git -C "$WORK_SRC" config user.name "SmartGateway Build"
git -C "$WORK_SRC" add -A
git -C "$WORK_SRC" commit -q -m "base $(git -C "$UBOOT_SRC" rev-parse --short HEAD)"
git -C "$WORK_SRC" apply --check "$PATCH_FILE"
git -C "$WORK_SRC" apply "$PATCH_FILE"

echo "=== Configuring mx6ull_14x14_evk_defconfig ==="
make -C "$WORK_SRC" ARCH=arm CROSS_COMPILE="$LINARO_CROSS" mx6ull_14x14_evk_defconfig

echo "=== Building u-boot-dtb.imx ==="
make -C "$WORK_SRC" -j"$JOBS" ARCH=arm CROSS_COMPILE="$LINARO_CROSS" u-boot-dtb.imx System.map u-boot.cfg

cp "$WORK_SRC/u-boot-dtb.imx" "$ARM_OUT/u-boot-dtb.imx"
cp "$WORK_SRC/u-boot-dtb.bin" "$ARM_OUT/u-boot-dtb.bin"
cp "$WORK_SRC/u-boot.cfg" "$ARM_OUT/u-boot.cfg"
cp "$WORK_SRC/System.map" "$ARM_OUT/System.map"
cp "$WORK_SRC/.config" "$ARM_OUT/uboot.config"

echo "=== Building host fw_printenv / fw_setenv ==="
make -C "$WORK_SRC" ARCH=arm CC=gcc STRIP=strip env
cp "$WORK_SRC/tools/env/fw_printenv" "$HOST_OUT/fw_printenv"
ln -sf fw_printenv "$HOST_OUT/fw_setenv"

if [ -x "${BR_CROSS}gcc" ]; then
    echo "=== Building ARM fw_printenv / fw_setenv (Buildroot glibc 2.30 sysroot) ==="
    rm -f "$WORK_SRC/tools/env/"*.o "$WORK_SRC/tools/env/fw_printenv" "$WORK_SRC/tools/env/.strip"
    make -C "$WORK_SRC" ARCH=arm \
        CC="${BR_CROSS}gcc --sysroot=${BR_SYSROOT} -mcpu=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4" \
        STRIP="${BR_CROSS}strip" env
    cp "$WORK_SRC/tools/env/fw_printenv" "$ARM_OUT/fw_printenv"
    ln -sf fw_printenv "$ARM_OUT/fw_setenv"
fi

echo "=== Verifying compiled symbols and macros ==="
grep -E 'CONFIG_BOOTCOUNT_LIMIT|CONFIG_BOOTCOUNT_ENV|CONFIG_ENV_OFFSET_REDUND|CONFIG_HW_WATCHDOG|CONFIG_IMX_WATCHDOG|CONFIG_SYS_REDUNDAND_ENVIRONMENT' "$ARM_OUT/u-boot.cfg"
grep -E ' (bootcount_load|bootcount_store|hw_watchdog_init|hw_watchdog_reset|env_relocate_spec)$' "$ARM_OUT/System.map"

echo "=== Build completed ==="
ls -lh "$ARM_OUT/u-boot-dtb.imx" "$HOST_OUT/fw_printenv"