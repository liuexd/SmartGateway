#!/bin/sh
set -eu

PHASE="${1:-}"
TARGET_SLOT="${2:-}"

case "$TARGET_SLOT" in
    1|2) ;;
    *)
        echo "APPLY HOOK ERROR: invalid target slot '$TARGET_SLOT'" >&2
        exit 1
        ;;
esac

if [ "$PWD" = "/" ]; then
    echo "APPLY HOOK ERROR: refusing to operate in root '/'" >&2
    exit 1
fi

case "$PHASE" in
    preinst)
        if [ -L "$PWD/bundle" ]; then
            echo "APPLY HOOK ERROR: bundle staging path cannot be a symlink" >&2
            exit 1
        fi
        mkdir -p "$PWD/bundle"
        chmod 0700 "$PWD/bundle"
        rm -f "$PWD/bundle/manifest" "$PWD/bundle/manifest.sig" \
              "$PWD/bundle/rootfs.manifest" "$PWD/bundle/rootfs.img" \
              "$PWD/bundle/zImage" "$PWD/bundle/100ask_imx6ull-14x14.dtb"
        exit 0
        ;;
    postinst)
        if [ ! -d "$PWD/bundle" ] || [ -L "$PWD/bundle" ]; then
            echo "APPLY HOOK ERROR: missing or untrusted bundle staging directory" >&2
            exit 1
        fi
        for f in manifest manifest.sig rootfs.manifest rootfs.img zImage 100ask_imx6ull-14x14.dtb; do
            if [ ! -f "$PWD/bundle/$f" ] || [ -L "$PWD/bundle/$f" ]; then
                echo "APPLY HOOK ERROR: missing or non-regular staged file '$f'" >&2
                exit 1
            fi
        done
        chmod 0700 "$PWD/bundle"
        chmod 0600 "$PWD/bundle/"*

        # Strict root boundary: production root execution ignores all env-var overrides
        # and explicitly protects factory slot 2.
        if [ "$(id -u)" -eq 0 ]; then
            if [ "$TARGET_SLOT" = "2" ]; then
                echo "APPLY REJECTED: target slot 2 is write-protected factory baseline" >&2
                exit 1
            fi
            exec /usr/bin/sgw_bootctl apply-bundle \
                --bundle "$PWD/bundle" \
                --target "$TARGET_SLOT"
        fi

        # Unprivileged isolated host acceptance path (sgw_bootctl_isolated only).
        for var in SGW_SWU_ISOLATED_BOOTCTL SGW_SWU_POLICY SGW_SWU_SLOT_IMG \
                   SGW_SWU_FW_CFG SGW_SWU_CMDLINE SGW_SWU_MOUNTS \
                   SGW_SWU_MOUNTINFO SGW_SWU_SYSROOT; do
            eval "val=\${$var:-}"
            if [ -z "$val" ]; then
                echo "APPLY HOOK ERROR: unprivileged host run missing required $var" >&2
                exit 1
            fi
        done
        if [ ! -x "$SGW_SWU_ISOLATED_BOOTCTL" ]; then
            echo "APPLY HOOK ERROR: SGW_SWU_ISOLATED_BOOTCTL not executable" >&2
            exit 1
        fi

        set -- \
            -c "$SGW_SWU_FW_CFG" \
            --cmdline "$SGW_SWU_CMDLINE" \
            --mounts "$SGW_SWU_MOUNTS" \
            --sysroot "$SGW_SWU_SYSROOT" \
            apply-bundle \
            --bundle "$PWD/bundle" \
            --target "$TARGET_SLOT" \
            --policy "$SGW_SWU_POLICY" \
            --slot-img "$SGW_SWU_SLOT_IMG" \
            --mountinfo "$SGW_SWU_MOUNTINFO"
        if [ "${SGW_SWU_FUSE_DATA:-0}" = "1" ]; then
            set -- "$@" --fuse-data
        fi
        if [ -n "${SGW_SWU_EXPECTED_TARGET:-}" ]; then
            set -- "$@" --expected-target "$SGW_SWU_EXPECTED_TARGET"
        fi
        exec "$SGW_SWU_ISOLATED_BOOTCTL" "$@"
        ;;
    *)
        echo "APPLY HOOK ERROR: unsupported script phase '$PHASE'" >&2
        exit 1
        ;;
esac
