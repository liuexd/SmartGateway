#!/usr/bin/env bash
#
# Safely migrate an existing single-copy U-Boot env (0xE0000, 4-byte CRC header)
# to the SmartGateway redundant A/B env layout (0xE0000 + 0xE2000, 5-byte header),
# preserving board-unique variables such as ethaddr/eth1addr/panel/update.
#
set -euo pipefail

SINGLE_PRINTENV=""
SINGLE_CFG=""
REDUND_SETENV=""
REDUND_CFG=""
BACKUP_TXT=""

usage() {
    cat <<'EOF'
Usage: migrate_single_to_redund_env.sh \
  --single-printenv <path> --single-cfg <path> \
  --redund-setenv <path> --redund-cfg <path> \
  [--backup-out <path>]
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --single-printenv) SINGLE_PRINTENV="${2:-}"; shift 2 ;;
        --single-cfg) SINGLE_CFG="${2:-}"; shift 2 ;;
        --redund-setenv) REDUND_SETENV="${2:-}"; shift 2 ;;
        --redund-cfg) REDUND_CFG="${2:-}"; shift 2 ;;
        --backup-out) BACKUP_TXT="${2:-}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "ERROR: unknown arg $1" >&2; exit 2 ;;
    esac
done

if [ -z "$SINGLE_PRINTENV" ] || [ -z "$SINGLE_CFG" ] || [ -z "$REDUND_SETENV" ] || [ -z "$REDUND_CFG" ]; then
    usage >&2
    exit 2
fi

TMP_DUMP="$(mktemp)"
TMP_SCRIPT="$(mktemp)"
trap 'rm -f "$TMP_DUMP" "$TMP_SCRIPT" "${TMP_DUMP}.err"' EXIT

# 1. Read and verify valid CRC using legacy single-env tool (refuse if legacy CRC is bad)
"$SINGLE_PRINTENV" -c "$SINGLE_CFG" > "$TMP_DUMP" 2>"${TMP_DUMP}.err"
if grep -qi "Bad CRC" "${TMP_DUMP}.err"; then
    cat "${TMP_DUMP}.err" >&2
    echo "ERROR: legacy single-copy environment failed CRC; refusing unsafe migration" >&2
    exit 1
fi

if [ -n "$BACKUP_TXT" ]; then
    cp "$TMP_DUMP" "$BACKUP_TXT"
fi

# 2. Build redundant batch script preserving identity/SWUpdate vars and enforcing safe p2 A/B defaults
python3 - "$TMP_DUMP" "$TMP_SCRIPT" <<'PYEOF'
import sys
dump_path, script_path = sys.argv[1], sys.argv[2]
preserve_keys = ("ethaddr", "eth1addr", "panel", "console", "baudrate", "fdt_file", "update")
kv = {}
with open(dump_path, "r", encoding="utf-8") as f:
    for line in f:
        if "=" in line:
            k, v = line.rstrip("\n").split("=", 1)
            kv[k] = v

lines = []
for k in preserve_keys:
    if k in kv and kv[k]:
        lines.append(f"{k} {kv[k]}")

lines.extend([
    "sgw_active_slot 2",
    "sgw_last_good_slot 2",
    "sgw_pending_slot none",
    "sgw_ab_state confirmed",
    "bootlimit 3",
    "bootcount 0",
    "upgrade_available 0",
    "sgw_rollback_count 0",
    "sgw_last_error none",
])
with open(script_path, "w", encoding="utf-8") as f:
    f.write("\n".join(lines) + "\n")
PYEOF

# 3. Write twice to redundant env so BOTH 0xE0000 and 0xE2000 copies carry valid 5-byte CRC32+flags
"$REDUND_SETENV" -c "$REDUND_CFG" -s "$TMP_SCRIPT" >/dev/null 2>&1
printf "sgw_active_slot 2\n" | "$REDUND_SETENV" -c "$REDUND_CFG" -s - >/dev/null 2>&1
echo "Migration to redundant environment completed cleanly."
