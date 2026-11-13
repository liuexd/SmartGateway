#!/usr/bin/env bash
set -euo pipefail

PASS=0
FAIL=0

check() {
    local label="$1"
    shift
    if "$@"; then
        echo "[PASS] $label"
        PASS=$((PASS + 1))
    else
        echo "[FAIL] $label"
        FAIL=$((FAIL + 1))
    fi
}

REPO_DIR="${SGW_BSP_DIR:-$HOME/Desktop/project/SmartGateway-BSP}"
APP_REPO_DIR="${SGW_APP_DIR:-$HOME/Desktop/project/SmartGateway}"
UBOOT_DIR="${UBOOT_SRC_DIR:-$HOME/Desktop/Uboot-2017.03}"
LINUX_DIR="${LINUX_SRC_DIR:-$HOME/Desktop/Linux-4.9.88}"
BR_DIR="${BUILDROOT_SRC_DIR:-$HOME/Desktop/Buildroot_2020.02.x}"
LINARO_CC="${LINARO_CC:-$HOME/Desktop/ToolChain/gcc-linaro-6.2.1-2016.11-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-gcc}"
BR_CC="${BR_CC:-$HOME/Desktop/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot/bin/arm-buildroot-linux-gnueabihf-gcc}"

echo "=== SmartGateway-BSP G1 Host Baseline Check ==="
echo "Date (UTC): $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
echo "Host: $(uname -a)"
echo

check "SmartGateway-BSP exists" test -d "$REPO_DIR/.git"
BRANCH="$(git -C "$REPO_DIR" branch --show-current 2>/dev/null || true)"
echo "SmartGateway-BSP branch: $BRANCH (HEAD $(git -C "$REPO_DIR" rev-parse --short HEAD))"
check "SmartGateway-BSP on feature/imx6ull-system-ab-ota" test "$BRANCH" = "feature/imx6ull-system-ab-ota"

if test -d "$APP_REPO_DIR/.git"; then
    APP_BRANCH="$(git -C "$APP_REPO_DIR" branch --show-current 2>/dev/null || true)"
    echo "SmartGateway branch: $APP_BRANCH (HEAD $(git -C "$APP_REPO_DIR" rev-parse --short HEAD))"
    check "SmartGateway on main" test "$APP_BRANCH" = "main"
fi

check "U-Boot 2017.03 source exists" test -d "$UBOOT_DIR/drivers/bootcount"
echo "U-Boot commit: $(git -C "$UBOOT_DIR" rev-parse --short HEAD) (dirty=$(git -C "$UBOOT_DIR" status --porcelain | wc -l))"
check "U-Boot working tree clean" test -z "$(git -C "$UBOOT_DIR" status --porcelain)"

check "Linux 4.9.88 source exists" test -d "$LINUX_DIR/arch/arm"
echo "Linux commit: $(git -C "$LINUX_DIR" rev-parse --short HEAD) (modified=$(git -C "$LINUX_DIR" status --porcelain | wc -l))"

check "Buildroot 2020.02.x source exists" test -d "$BR_DIR/package/swupdate"
echo "Buildroot commit: $(git -C "$BR_DIR" rev-parse --short HEAD)"

check "Linaro GCC 6.2.1 executable" test -x "$LINARO_CC"
echo "Linaro GCC: $($LINARO_CC --version | head -n1)"

check "Buildroot SDK GCC executable" test -x "$BR_CC"
echo "Buildroot GCC: $($BR_CC --version | head -n1)"

check "Host gcc available" command -v gcc >/dev/null
check "Host python3 available" command -v python3 >/dev/null
check "Host make available" command -v make >/dev/null

echo
echo "Summary: PASS=$PASS FAIL=$FAIL"
test "$FAIL" -eq 0