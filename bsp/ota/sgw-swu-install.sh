#!/bin/sh
set -eu

if [ "$#" -ne 1 ] || [ ! -f "$1" ]; then
    echo "Usage: $0 <package.swu>" >&2
    exit 2
fi

SWU_DIR=$(CDPATH= cd -- "$(dirname -- "$1")" && pwd)
SWU_ABS="$SWU_DIR/$(basename -- "$1")"

BOARD="imx6ull-100ask"
HWREV="revA"
if [ -r /etc/hwrevision ]; then
    read -r file_board file_rev _rest < /etc/hwrevision || true
    if [ -n "${file_board:-}" ] && [ -n "${file_rev:-}" ]; then
        BOARD="$file_board"
        HWREV="$file_rev"
    fi
fi

# Strict root boundary: ignore env overrides when running as root.
if [ "$(id -u)" -eq 0 ]; then
    PUBKEY="/etc/smartgateway/ota-public.pem"
    WORK_BASE="/run"
else
    PUBKEY="${SGW_SWU_PUBKEY:-/etc/smartgateway/ota-public.pem}"
    WORK_BASE="${SGW_SWU_WORK_BASE:-/run}"
fi

WORKDIR=$(mktemp -d "$WORK_BASE/sgw-swu.XXXXXX")
cleanup() {
    rm -rf "$WORKDIR"
}
trap cleanup EXIT INT TERM

chmod 0700 "$WORKDIR"
mkdir -p "$WORKDIR/t" "$WORKDIR/bundle"
chmod 0700 "$WORKDIR/t" "$WORKDIR/bundle"

export TMPDIR="$WORKDIR/t"
cd "$WORKDIR"

rc=0
swupdate -v \
    -f /etc/swupdate/swupdate.cfg \
    -i "$SWU_ABS" \
    -k "$PUBKEY" \
    -H "${BOARD}:${HWREV}" \
    -M || rc=$?
exit "$rc"
