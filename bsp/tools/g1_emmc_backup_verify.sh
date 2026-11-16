#!/bin/sh
# Fail closed when Python 3 or the audited helper is unavailable.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
if ! command -v python3 >/dev/null 2>&1; then
    echo 'ERROR: python3 is required for block identity verification' >&2
    exit 1
fi
exec python3 "$SCRIPT_DIR/g1_emmc_backup_verify.py" "$@"
