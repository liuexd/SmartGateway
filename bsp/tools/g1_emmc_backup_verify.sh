#!/bin/sh
#
# SmartGateway BSP G1: Read-only eMMC backup and SHA-256 verification helper.
#
# Strictly READ-ONLY on /dev/mmcblk1. Refuses to write backup artifacts onto
# /dev/mmcblk1 itself. Full partition backup of p2 requires either running from
# an external recovery medium (SD/NFS) or having p2 mounted read-only, unless
# --allow-rw-snapshot is passed explicitly for non-consistent diagnostic capture.
#
# Usage:
#   sh g1_emmc_backup_verify.sh --dest /mnt/sd/sgw-backup [--mode metadata|full] [--allow-rw-snapshot]
#

set -eu

DEST=""
MODE="metadata"
ALLOW_RW=0
EMMC_DEV="${EMMC_DEV:-/dev/mmcblk1}"

usage() {
    cat <<'EOF'
Usage: g1_emmc_backup_verify.sh --dest <external_dir> [--mode metadata|full] [--allow-rw-snapshot]
  --dest <dir>          Destination directory on external storage (SD/USB/NFS/tmpfs)
  --mode metadata       Export partition table, fw_printenv, and pre-p1 4098 sectors (default)
  --mode full           Also backup p1, p2, p3, boot0, boot1 and verify SHA-256
  --allow-rw-snapshot   Allow p2 read even if mounted rw (diagnostic only; not crash-consistent)
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --dest)
            DEST="${2:-}"
            shift 2
            ;;
        --mode)
            MODE="${2:-}"
            shift 2
            ;;
        --allow-rw-snapshot)
            ALLOW_RW=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "ERROR: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [ -z "$DEST" ]; then
    echo "ERROR: --dest is required" >&2
    exit 2
fi

case "$MODE" in
    metadata|full) ;;
    *)
        echo "ERROR: invalid --mode '$MODE' (expected metadata or full)" >&2
        exit 2
        ;;
esac

if [ ! -b "$EMMC_DEV" ]; then
    echo "ERROR: block device not found: $EMMC_DEV" >&2
    exit 1
fi

mkdir -p "$DEST"
DEST_REAL="$(cd "$DEST" && pwd -P)"

# Refuse destination backed by mmcblk1
if command -v df >/dev/null 2>&1; then
    DEST_SRC="$(df -P "$DEST_REAL" | awk 'NR==2 {print $1}')"
    case "$DEST_SRC" in
        /dev/mmcblk1*)
            echo "ERROR: destination $DEST_REAL is backed by $DEST_SRC (must be off-eMMC)" >&2
            exit 1
            ;;
    esac
fi

echo "=== SmartGateway G1 Read-Only Backup ==="
echo "Device : $EMMC_DEV"
echo "Dest   : $DEST_REAL"
echo "Mode   : $MODE"

if command -v sfdisk >/dev/null 2>&1; then
    sfdisk -d "$EMMC_DEV" > "$DEST_REAL/emmc-partition-table.sfdisk"
fi
if command -v fdisk >/dev/null 2>&1; then
    fdisk -l "$EMMC_DEV" > "$DEST_REAL/emmc-fdisk.txt"
fi
if command -v fw_printenv >/dev/null 2>&1; then
    fw_printenv > "$DEST_REAL/uboot-env.txt" 2>"$DEST_REAL/uboot-env.err" || true
fi
cp /proc/partitions "$DEST_REAL/proc-partitions.txt" 2>/dev/null || true
cp /proc/mounts "$DEST_REAL/proc-mounts.txt" 2>/dev/null || true

# Pre-p1 reserved area (sectors 0..4097 = 4098 sectors = 2,098,176 bytes)
dd if="$EMMC_DEV" of="$DEST_REAL/emmc-reserved-pre-p1.bin" bs=512 count=4098 conv=sync,noerror

if [ "$MODE" = "full" ]; then
    P2_RW="$(awk '$1 ~ /mmcblk1p2|\/dev\/root/ && $2 == "/" && $4 ~ /(^|,)rw(,|$)/ {print $1}' /proc/mounts || true)"
    if [ -n "$P2_RW" ] && [ "$ALLOW_RW" -ne 1 ]; then
        echo "ERROR: /dev/mmcblk1p2 is mounted rw at / ($P2_RW). Run full backup from SD recovery boot or remount ro." >&2
        exit 1
    fi

    for part in p1 p2 p3; do
        pdev="${EMMC_DEV}${part}"
        if [ -b "$pdev" ]; then
            echo "Backing up $pdev ..."
            dd if="$pdev" of="$DEST_REAL/emmc-${part}.img" bs=1M
        fi
    done

    for bpart in boot0 boot1; do
        bdev="${EMMC_DEV}${bpart}"
        if [ -b "$bdev" ]; then
            echo "Backing up $bdev (read-only) ..."
            dd if="$bdev" of="$DEST_REAL/emmc-${bpart}.img" bs=512
        fi
    done
fi

(
    cd "$DEST_REAL"
    sha256sum ./*.bin ./*.img ./*.sfdisk ./*.txt 2>/dev/null > SHA256SUMS || true
    sha256sum -c SHA256SUMS
)

echo "Backup and SHA-256 verification completed in $DEST_REAL"