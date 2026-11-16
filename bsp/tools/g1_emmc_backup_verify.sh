#!/bin/sh
#
# SmartGateway BSP G1: Read-only eMMC backup and SHA-256 verification helper.
#
# Strictly READ-ONLY on /dev/mmcblk1. Refuses to create directories or write
# backup artifacts onto /dev/mmcblk1* or onto /dev/root backed by eMMC.
# Full partition backup of p2 requires running from an external recovery
# medium (SD/USB/NFS) or having p2 mounted read-only, unless
# --allow-rw-snapshot is passed explicitly for non-consistent capture.
#

set -eu

DEST=""
MODE="metadata"
ALLOW_RW=0
EMMC_DEV="${EMMC_DEV:-/dev/mmcblk1}"
PROC_MOUNTS="${PROC_MOUNTS:-/proc/mounts}"
PROC_CMDLINE="${PROC_CMDLINE:-/proc/cmdline}"

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

if [ ! -e "$EMMC_DEV" ]; then
    echo "ERROR: block device not found: $EMMC_DEV" >&2
    exit 1
fi
if [ ! -b "$EMMC_DEV" ] && [ "${SGW_ALLOW_FILE_BACKED_TEST:-0}" -ne 1 ]; then
    echo "ERROR: $EMMC_DEV is not a block device" >&2
    exit 1
fi

EMMC_REAL="$(readlink -f "$EMMC_DEV" 2>/dev/null || echo "$EMMC_DEV")"
EMMC_BASE="$(basename "$EMMC_REAL")"

# Resolve nearest existing ancestor BEFORE creating DEST so we never touch a forbidden FS.
PROBE_DIR="$DEST"
while [ ! -e "$PROBE_DIR" ]; do
    NEXT_DIR="$(dirname "$PROBE_DIR")"
    if [ "$NEXT_DIR" = "$PROBE_DIR" ]; then
        break
    fi
    PROBE_DIR="$NEXT_DIR"
done
if [ ! -d "$PROBE_DIR" ]; then
    echo "ERROR: destination parent directory does not exist: $PROBE_DIR" >&2
    exit 1
fi
DEST_PROBE_REAL="$(cd "$PROBE_DIR" && pwd -P)"

extract_cmdline_root() {
    if [ ! -r "$PROC_CMDLINE" ]; then
        return 0
    fi
    tr ' ' '\n' < "$PROC_CMDLINE" | awk -F= '$1=="root"{print $2; exit}'
}

is_proven_non_emmc_root() {
    root_arg="$(extract_cmdline_root)"
    case "$root_arg" in
        /dev/mmcblk0*|/dev/sd[a-z]*|/dev/nvme*|/dev/vd[a-z]*|/dev/nfs*|nfs)
            if [ -r "$PROC_MOUNTS" ] && awk -v base="$EMMC_BASE" '$2 == "/" && $1 ~ ("^/dev/" base) {found=1} END{exit found?0:1}' "$PROC_MOUNTS"; then
                return 1
            fi
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

if command -v df >/dev/null 2>&1; then
    DF_LINE="$(df -P "$DEST_PROBE_REAL" | awk 'NR==2 {print $0}')"
    DEST_SRC="$(printf '%s\n' "$DF_LINE" | awk '{print $1}')"
    DEST_MNT="$(printf '%s\n' "$DF_LINE" | awk '{print $NF}')"

    case "$DEST_SRC" in
        /dev/"$EMMC_BASE"*)
            echo "ERROR: destination $DEST_PROBE_REAL is backed by $DEST_SRC (must be off-eMMC)" >&2
            exit 1
            ;;
        /dev/root|rootfs)
            root_arg="$(extract_cmdline_root)"
            if ! is_proven_non_emmc_root; then
                echo "ERROR: destination $DEST_PROBE_REAL is on $DEST_SRC (root=${root_arg:-unknown}); refusing potential eMMC self-backup" >&2
                exit 1
            fi
            ;;
        "")
            echo "ERROR: unable to determine filesystem backing $DEST_PROBE_REAL" >&2
            exit 1
            ;;
    esac

    if [ -r "$PROC_MOUNTS" ] && [ -n "$DEST_MNT" ]; then
        MNT_DEV="$(awk -v mnt="$DEST_MNT" '$2 == mnt {dev=$1} END{print dev}' "$PROC_MOUNTS")"
        case "$MNT_DEV" in
            /dev/"$EMMC_BASE"*)
                echo "ERROR: mountpoint $DEST_MNT is backed by $MNT_DEV (must be off-eMMC)" >&2
                exit 1
                ;;
            /dev/root|rootfs)
                if ! is_proven_non_emmc_root; then
                    echo "ERROR: mountpoint $DEST_MNT is on $MNT_DEV (unproven off-eMMC root)" >&2
                    exit 1
                fi
                ;;
        esac
    fi
fi

mkdir -p "$DEST"
DEST_REAL="$(cd "$DEST" && pwd -P)"

echo "=== SmartGateway G1 Read-Only Backup ==="
echo "Device : $EMMC_DEV"
echo "Dest   : $DEST_REAL"
echo "Mode   : $MODE"

if command -v sfdisk >/dev/null 2>&1; then
    sfdisk -d "$EMMC_DEV" > "$DEST_REAL/emmc-partition-table.sfdisk" 2>/dev/null || true
fi
if command -v fdisk >/dev/null 2>&1; then
    fdisk -l "$EMMC_DEV" > "$DEST_REAL/emmc-fdisk.txt" 2>/dev/null || true
fi
if command -v fw_printenv >/dev/null 2>&1; then
    fw_printenv > "$DEST_REAL/uboot-env.txt" 2>"$DEST_REAL/uboot-env.err" || true
fi
cp /proc/partitions "$DEST_REAL/proc-partitions.txt" 2>/dev/null || true
cp "$PROC_MOUNTS" "$DEST_REAL/proc-mounts.txt" 2>/dev/null || true

# Pre-p1 reserved area (sectors 0..4097 = 4098 sectors = 2,098,176 bytes)
dd if="$EMMC_DEV" of="$DEST_REAL/emmc-reserved-pre-p1.bin" bs=512 count=4098 conv=sync,noerror

if [ "$MODE" = "full" ]; then
    P2_RW="$(awk -v base="$EMMC_BASE" '
        ($1 ~ ("^/dev/" base "p2$") || $1 == "/dev/root") &&
        ($2 == "/") &&
        ($4 ~ /(^|,)rw(,|$)/) { print $1; exit }
    ' "$PROC_MOUNTS" 2>/dev/null || true)"

    if [ -n "$P2_RW" ] && [ "$ALLOW_RW" -ne 1 ]; then
        if [ "$P2_RW" != "/dev/root" ] || ! is_proven_non_emmc_root; then
            echo "ERROR: /dev/${EMMC_BASE}p2 is mounted rw at / ($P2_RW). Run full backup from external SD recovery boot or remount ro." >&2
            exit 1
        fi
    fi

    for part in p1 p2 p3; do
        pdev="${EMMC_DEV}${part}"
        if [ -b "$pdev" ] || { [ "${SGW_ALLOW_FILE_BACKED_TEST:-0}" -eq 1 ] && [ -f "$pdev" ]; }; then
            echo "Backing up $pdev ..."
            dd if="$pdev" of="$DEST_REAL/emmc-${part}.img" bs=1M
        fi
    done

    for bpart in boot0 boot1; do
        bdev="${EMMC_DEV}${bpart}"
        if [ -b "$bdev" ] || { [ "${SGW_ALLOW_FILE_BACKED_TEST:-0}" -eq 1 ] && [ -f "$bdev" ]; }; then
            echo "Backing up $bdev (read-only) ..."
            dd if="$bdev" of="$DEST_REAL/emmc-${bpart}.img" bs=512
        fi
    done
fi

(
    cd "$DEST_REAL"
    : > SHA256SUMS
    for f in *.bin *.img *.sfdisk *.txt; do
        [ -f "$f" ] && sha256sum "$f" >> SHA256SUMS
    done
    sha256sum -c SHA256SUMS
)

echo "Backup and SHA-256 verification completed in $DEST_REAL"
