#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
LINUX_SRC="${LINUX_SRC_DIR:-$HOME/Desktop/Linux-4.9.88}"
AP3216_PATCH="$REPO_DIR/bsp/dts/patches/0001-add-ap3216-device-tree-node.patch"
GPIO_EVENT_PATCH="$REPO_DIR/bsp/dts/patches/0002-add-sgw-gpio-event-device-tree-node.patch"
CMA_PATCH="$SCRIPT_DIR/patches/0001-dts-trim-cma-from-320m-to-32m.patch"
FRAGMENT="$SCRIPT_DIR/g3_trimmed.fragment"
OUT_ROOT="${KERNEL_G3_BUILD_DIR:-$REPO_DIR/build/kernel-g3}"

LINARO_CROSS="${LINARO_CROSS:-$HOME/Desktop/ToolChain/gcc-linaro-6.2.1-2016.11-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-}"
JOBS="${JOBS:-$(nproc)}"

if [ ! -d "$LINUX_SRC/.git" ]; then
    echo "ERROR: Linux 4.9.88 repo not found at $LINUX_SRC" >&2
    exit 1
fi
if [ ! -x "${LINARO_CROSS}gcc" ]; then
    echo "ERROR: Linaro toolchain not found at ${LINARO_CROSS}gcc" >&2
    exit 1
fi

build_variant() {
    local variant="$1"
    local work="$OUT_ROOT/$variant/src"
    local out="$OUT_ROOT/$variant/out"
    local mods="$OUT_ROOT/$variant/mods"
    local bsp_ko="$OUT_ROOT/$variant/bsp-ko"

    echo "=== Preparing $variant kernel source in $work ==="
    rm -rf "$work" "$out" "$mods" "$bsp_ko"
    mkdir -p "$work" "$out" "$mods" "$bsp_ko"

    git -C "$LINUX_SRC" archive HEAD | tar -x -C "$work"
    git -C "$work" init -q
    git -C "$work" config user.email "build@smartgateway.local"
    git -C "$work" config user.name "SmartGateway Build"
    git -C "$work" add -A
    git -C "$work" commit -q -m "base $(git -C "$LINUX_SRC" rev-parse --short HEAD)"

    # Apply existing ap3216@1e DTS patch so both variants include project DTS baseline
    git -C "$work" apply "$AP3216_PATCH"
    git -C "$work" apply "$GPIO_EVENT_PATCH"

    make -C "$work" ARCH=arm CROSS_COMPILE="$LINARO_CROSS" 100ask_imx6ull_defconfig >/dev/null

    if [ "$variant" = "trimmed" ]; then
        git -C "$work" apply "$CMA_PATCH"
        (cd "$work" && ARCH=arm CROSS_COMPILE="$LINARO_CROSS" ./scripts/kconfig/merge_config.sh -m .config "$FRAGMENT" >/dev/null)
        make -C "$work" ARCH=arm CROSS_COMPILE="$LINARO_CROSS" olddefconfig >/dev/null
    fi

    echo "=== Compiling $variant zImage, DTB, and modules ==="
    local t0 t1
    t0="$(date +%s)"
    sed -i "s/^YYLTYPE yylloc;/extern YYLTYPE yylloc;/" "$work/scripts/dtc/dtc-lexer.lex.c_shipped"
    make -C "$work" -j"$JOBS" ARCH=arm HOSTCC="gcc -fcommon" CROSS_COMPILE="$LINARO_CROSS" zImage dtbs modules
    make -C "$work" ARCH=arm CROSS_COMPILE="$LINARO_CROSS" INSTALL_MOD_PATH="$mods" modules_install >/dev/null
    t1="$(date +%s)"

    cp "$work/arch/arm/boot/zImage" "$out/zImage"
    cp "$work/arch/arm/boot/dts/100ask_imx6ull-14x14.dtb" "$out/100ask_imx6ull-14x14.dtb"
    cp "$work/.config" "$out/kernel.config"
    cp "$work/vmlinux" "$out/vmlinux"
    "${LINARO_CROSS}size" "$work/vmlinux" > "$out/vmlinux.size"
    echo "$((t1 - t0))" > "$out/build_seconds"

    # Build external SmartGateway BSP kernel modules against this variant
    for mod in hello sgw_ap3216 sgw_gpio_event devm_demo dt_match_demo; do
        local mtmp="$OUT_ROOT/$variant/tmp_${mod}"
        rm -rf "$mtmp"
        cp -a "$REPO_DIR/bsp/drivers/$mod" "$mtmp"
        make -C "$work" ARCH=arm CROSS_COMPILE="$LINARO_CROSS" M="$mtmp" modules >/dev/null
        cp "$mtmp"/*.ko "$bsp_ko/"
        rm -rf "$mtmp"
    done

    # Assert critical drivers/symbols are present in compiled .config & vmlinux
    for sym in \
        CONFIG_SOC_IMX6ULL=y CONFIG_IMX2_WDT=y \
        CONFIG_MMC_SDHCI_ESDHC_IMX=y CONFIG_EXT4_FS=y \
        CONFIG_CONFIGFS_FS=y CONFIG_USB_CONFIGFS=y CONFIG_USB_CONFIGFS_F_FS=y \
        CONFIG_FEC=y CONFIG_WLAN=y CONFIG_CFG80211=y \
        CONFIG_GPIOLIB=y CONFIG_GPIO_MXC=y CONFIG_I2C_IMX=y CONFIG_SPI_IMX=y \
        CONFIG_SERIAL_IMX=y CONFIG_MODULES=y CONFIG_CRYPTO_SHA256=y \
        CONFIG_PERF_EVENTS=y CONFIG_HW_PERF_EVENTS=y; do
        if ! grep -qx "$sym" "$out/kernel.config"; then
            echo "ERROR: mandatory option missing in $variant: $sym" >&2
            exit 1
        fi
    done

    # Clean heavy intermediate .o files in src after artifacts & external .ko builds complete (preserve headers/scripts for inspection)
    rm -rf "$work"
}

if [ "${ONLY_VARIANT:-all}" = "all" ] || [ "${ONLY_VARIANT:-all}" = "baseline" ]; then
    build_variant baseline
fi
if [ "${ONLY_VARIANT:-all}" = "all" ] || [ "${ONLY_VARIANT:-all}" = "trimmed" ]; then
    build_variant trimmed
fi

diff -u "$OUT_ROOT/baseline/out/kernel.config" "$OUT_ROOT/trimmed/out/kernel.config" > "$OUT_ROOT/g3_kernel_config.diff" || true

python3 - "$OUT_ROOT" <<'PYEOF'
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

out_root = Path(sys.argv[1])
metrics = {}
for variant in ("baseline", "trimmed"):
    vdir = out_root / variant / "out"
    mdir = out_root / variant / "mods"
    kdir = out_root / variant / "bsp-ko"
    ko_files = list(mdir.rglob("*.ko"))
    mods_bytes = sum(p.stat().st_size for p in ko_files)
    bsp_ko_files = sorted(p.name for p in kdir.glob("*.ko"))

    dtb_path = vdir / "100ask_imx6ull-14x14.dtb"
    cma_hex = subprocess.run(["fdtget", "-t", "x", str(dtb_path), "/reserved-memory/linux,cma", "size"], capture_output=True, text=True, check=True).stdout.strip()
    ap_compat = subprocess.run(["fdtget", str(dtb_path), "/soc/aips-bus@02100000/i2c@021a0000/ap3216@1e", "compatible"], capture_output=True, text=True, check=True).stdout.strip()
    gpio_compat = subprocess.run(["fdtget", str(dtb_path), "/sgw-gpio-event", "compatible"], capture_output=True, text=True, check=True).stdout.strip()

    size_lines = (vdir / "vmlinux.size").read_text(encoding="utf-8").splitlines()
    text_b = data_b = bss_b = dec_b = 0
    if len(size_lines) >= 2:
        parts = size_lines[1].split()
        text_b, data_b, bss_b, dec_b = int(parts[0]), int(parts[1]), int(parts[2]), int(parts[3])

    cfg_lines = (vdir / "kernel.config").read_text(encoding="utf-8").splitlines()
    enabled_y = sum(1 for l in cfg_lines if l.endswith("=y"))
    enabled_m = sum(1 for l in cfg_lines if l.endswith("=m"))

    metrics[variant] = {
        "zImage_bytes": (vdir / "zImage").stat().st_size,
        "dtb_bytes": dtb_path.stat().st_size,
        "vmlinux_bytes": (vdir / "vmlinux").stat().st_size,
        "vmlinux_text": text_b,
        "vmlinux_data": data_b,
        "vmlinux_bss": bss_b,
        "vmlinux_dec": dec_b,
        "modules_count": len(ko_files),
        "modules_bytes": mods_bytes,
        "bsp_ko_built": bsp_ko_files,
        "config_y": enabled_y,
        "config_m": enabled_m,
        "cma_bytes": int(cma_hex, 16),
        "cma_mib": int(cma_hex, 16) // (1024 * 1024),
        "ap3216_in_dtb": (ap_compat == "smartgateway,ap3216"),
        "gpio_in_dtb": (gpio_compat == "smartgateway,sgw-gpio-event"),
        "build_seconds": int((vdir / "build_seconds").read_text(encoding="utf-8").strip()),
    }

(out_root / "g3_kernel_metrics.json").write_text(json.dumps(metrics, indent=2), encoding="utf-8")
print(json.dumps(metrics, indent=2))
PYEOF
