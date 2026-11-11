#!/bin/sh
#
# SmartGateway BSP: read-only i.MX6ULL eMMC inventory for G0-1.
#
# This script only reads target metadata and writes its report to /tmp.
# It does NOT mount partitions, modify partition tables, write eMMC,
# change U-Boot environment, run filesystem repair, or erase anything.
#
# Run on the target board:
#   sh /tmp/collect_system_baseline.sh
#
# Report:
#   /tmp/sgw-g0-readonly-inventory.txt
#

umask 077
OUT="/tmp/sgw-g0-readonly-inventory.txt"

if ! : > "$OUT"; then
    echo "ERROR: cannot create report at $OUT"
    exit 1
fi

exec > "$OUT" 2>&1

section()
{
    echo
    echo "========== $1 =========="
}

optional()
{
    cmd="$1"
    shift
    if command -v "$cmd" >/dev/null 2>&1; then
        echo "+ $cmd $*"
        "$cmd" "$@"
        rc=$?
        echo "[exit=$rc]"
    else
        echo "[not installed] $cmd"
    fi
}

section "COLLECTION META"
date 2>/dev/null || true
echo "report=$OUT"
echo "NOTE: eMMC CID/serial is intentionally not printed."

section "SYSTEM"
uname -a
cat /proc/cmdline 2>/dev/null
cat /proc/version 2>/dev/null

section "BLOCK DEVICES"
cat /proc/partitions 2>/dev/null
ls -l /dev/mmcblk* 2>/dev/null

section "EMMC DEVICE IDENTITY (NON-UNIQUE FIELDS)"
for f in type name manfid oemid serial date; do
    p="/sys/block/mmcblk1/device/$f"
    if [ -r "$p" ]; then
        if [ "$f" = "serial" ]; then
            echo "$f=[REDACTED]"
        else
            echo "$f=$(cat "$p" 2>/dev/null)"
        fi
    fi
done
echo "user_area_sectors=$(cat /sys/block/mmcblk1/size 2>/dev/null)"

section "PARTITION GEOMETRY (512-BYTE SECTORS)"
for p in /sys/class/block/mmcblk1p*; do
    [ -r "$p/start" ] || continue
    echo "PARTITION=$p"
    echo "START=$(cat "$p/start" 2>/dev/null)"
    echo "SIZE=$(cat "$p/size" 2>/dev/null)"
done

section "READ-ONLY PARTITION TABLE INSPECTION"
optional fdisk -l /dev/mmcblk1
optional sfdisk -d /dev/mmcblk1
optional parted -s /dev/mmcblk1 unit s print

section "FILESYSTEM SIGNATURES"
optional blkid /dev/mmcblk1p1 /dev/mmcblk1p2 /dev/mmcblk1p3
optional wipefs -n /dev/mmcblk1p1
optional wipefs -n /dev/mmcblk1p2
optional wipefs -n /dev/mmcblk1p3

section "MOUNTS AND SPACE"
cat /proc/mounts 2>/dev/null
df -hT 2>/dev/null || df -h 2>/dev/null
echo "--- eMMC mount references ---"
grep 'mmcblk1' /proc/mounts 2>/dev/null || true

section "BOOT DIRECTORY (READ ONLY)"
ls -lah /boot 2>&1

section "EMMC HARDWARE BOOT PARTITION FLAGS"
for p in /sys/block/mmcblk1boot0/force_ro /sys/block/mmcblk1boot1/force_ro; do
    if [ -r "$p" ]; then
        echo "$p=$(cat "$p" 2>/dev/null)"
    fi
done

section "RELEVANT KERNEL LOG"
dmesg 2>/dev/null | grep -Ei 'mmc|ext[234]|partition|cma|reserved memory|Memory:' || true

section "END"
echo "Inventory completed. Report is stored at $OUT"
echo "No eMMC writes or partition modifications were performed."

echo "Read-only inventory saved to: $OUT" >&2
