#!/usr/bin/env bash
set -euo pipefail

TARGET_DIR="$1"
REPO_DIR="${SGW_BSP_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
BR_OVERLAY="${SGW_VENDOR_OVERLAY:-$HOME/Desktop/Buildroot_2020.02.x/board/100ask/nxp-imx6ull/rootfs-overlay/systemV}"
K3_OUT="$REPO_DIR/build/kernel-g3/trimmed/out"
K3_MODS="$REPO_DIR/build/kernel-g3/trimmed/mods"
K3_BSP_KO="$REPO_DIR/build/kernel-g3/trimmed/bsp-ko"

echo "=== SmartGateway G3 post-build hook ($TARGET_DIR) ==="

# 1. Clean bloat directories that may come from defaults or previous runs
rm -rf "$TARGET_DIR/usr/share/lvgl" "$TARGET_DIR/etc/pulse" "$TARGET_DIR/etc/samba" "$TARGET_DIR/etc/bluetooth" \
       "$TARGET_DIR/etc/udev/hwdb.bin" "$TARGET_DIR/etc/udev/hwdb.d"
rm -f "$TARGET_DIR/etc/php.ini" "$TARGET_DIR/etc/init.d/S05lvgl" "$TARGET_DIR/etc/init.d/S50telnet"

# 2. Install essential board config & init scripts from vendor overlay (without bloat)
mkdir -p "$TARGET_DIR/etc/init.d" "$TARGET_DIR/etc/network" "$TARGET_DIR/etc/swupdate" \
         "$TARGET_DIR/etc/udev/rules.d" "$TARGET_DIR/usr/bin" "$TARGET_DIR/sbin" "$TARGET_DIR/boot"

if [ -d "$BR_OVERLAY" ]; then
    cp -f "$BR_OVERLAY/etc/init.d/S99adbd" "$TARGET_DIR/etc/init.d/S99adbd"
    cp -f "$BR_OVERLAY/etc/init.d/S50sshd" "$TARGET_DIR/etc/init.d/S50sshd"
    cp -f "$BR_OVERLAY/usr/bin/usb_config" "$TARGET_DIR/usr/bin/usb_config"
    cp -f "$BR_OVERLAY/etc/udev/rules.d/61-usb-adbd.rules" "$TARGET_DIR/etc/udev/rules.d/61-usb-adbd.rules"
    cp -f "$BR_OVERLAY/etc/network/interfaces" "$TARGET_DIR/etc/network/interfaces"
    cp -f "$BR_OVERLAY/etc/hwrevision" "$TARGET_DIR/etc/hwrevision"
    cp -f "$BR_OVERLAY/etc/sw-versions" "$TARGET_DIR/etc/sw-versions"
    cp -f "$BR_OVERLAY/etc/swupdate/swupdate.cfg" "$TARGET_DIR/etc/swupdate/swupdate.cfg"
    cp -f "$BR_OVERLAY/etc/swupdate/swupdate-public.pem" "$TARGET_DIR/etc/swupdate/swupdate-public.pem"
    chmod 755 "$TARGET_DIR/etc/init.d/S99adbd" "$TARGET_DIR/etc/init.d/S50sshd" "$TARGET_DIR/usr/bin/usb_config"
fi

# 3. Corrected /etc/fstab (ext4 matches actual runtime root mount)
cat > "$TARGET_DIR/etc/fstab" <<'EOF'
# <file system>	<mount pt>	<type>	<options>	<dump>	<pass>
/dev/root	/		ext4	rw,noauto	0	1
proc		/proc		proc	defaults	0	0
devpts		/dev/pts	devpts	defaults,gid=5,mode=620,ptmxmode=0666	0	0
tmpfs		/dev/shm	tmpfs	mode=0777	0	0
tmpfs		/tmp		tmpfs	mode=1777	0	0
tmpfs		/run		tmpfs	mode=0755,nosuid,nodev	0	0
sysfs		/sys		sysfs	defaults	0	0
EOF

# 4. Clean SysV rcS (without psplash) & clean /etc/profile (without Qt env bloat)
cat > "$TARGET_DIR/etc/init.d/rcS" <<'EOF'
#!/bin/sh
for i in /etc/init.d/S??* ; do
    [ ! -f "$i" ] && continue
    case "$i" in
        *.sh)
            (
                trap - INT QUIT TSTP
                set start
                . "$i"
            )
            ;;
        *)
            "$i" start
            ;;
    esac
done
/bin/hostname -F /etc/hostname 2>/dev/null || true
EOF
chmod 755 "$TARGET_DIR/etc/init.d/rcS"

cat > "$TARGET_DIR/etc/profile" <<'EOF'
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
if [ "$PS1" ]; then
    if [ "$(id -u)" -eq 0 ]; then
        export PS1='# '
    else
        export PS1='$ '
    fi
fi
export PAGER='/bin/more'
export EDITOR='/bin/vi'
EOF

# 5. Redundant U-Boot env config + A/B tools + Service Supervisor + S15/S99 init scripts
cp -f "$REPO_DIR/bsp/uboot/fw_env.ab.config" "$TARGET_DIR/etc/fw_env.config"
cp -f "$REPO_DIR/bsp/apps/boot_ab/S15sgw_ab_guard" "$TARGET_DIR/etc/init.d/S15sgw_ab_guard"
chmod 755 "$TARGET_DIR/etc/init.d/S15sgw_ab_guard"

if [ -x "$REPO_DIR/bsp/apps/service_supervisor/S99smartgateway" ]; then
    cp -f "$REPO_DIR/bsp/apps/service_supervisor/S99smartgateway" "$TARGET_DIR/etc/init.d/S99smartgateway"
    chmod 755 "$TARGET_DIR/etc/init.d/S99smartgateway"
fi

for bin in sgw_bootctl sgw_watchdog_guard sgw_system_health; do
    if [ -x "$REPO_DIR/build/arm-ab/$bin" ]; then
        cp -f "$REPO_DIR/build/arm-ab/$bin" "$TARGET_DIR/usr/bin/$bin"
        chmod 755 "$TARGET_DIR/usr/bin/$bin"
    fi
done

if [ -x "$REPO_DIR/build/arm-s5/service_supervisor" ]; then
    cp -f "$REPO_DIR/build/arm-s5/service_supervisor" "$TARGET_DIR/usr/bin/service_supervisor"
    chmod 755 "$TARGET_DIR/usr/bin/service_supervisor"
fi
if [ -x "$REPO_DIR/build/arm-d25/smart_gateway" ]; then
    cp -f "$REPO_DIR/build/arm-d25/smart_gateway" "$TARGET_DIR/usr/bin/smart_gateway"
    chmod 755 "$TARGET_DIR/usr/bin/smart_gateway"
fi
if [ -x "$REPO_DIR/build/arm-d25/ota_agent" ]; then
    cp -f "$REPO_DIR/build/arm-d25/ota_agent" "$TARGET_DIR/usr/bin/ota_agent"
    chmod 755 "$TARGET_DIR/usr/bin/ota_agent"
fi

if [ -x "$REPO_DIR/build/uboot-ab/arm/fw_printenv" ]; then
    cp -f "$REPO_DIR/build/uboot-ab/arm/fw_printenv" "$TARGET_DIR/sbin/fw_printenv"
    ln -sf fw_printenv "$TARGET_DIR/sbin/fw_setenv"
    chmod 755 "$TARGET_DIR/sbin/fw_printenv"
    # Keep /usr/sbin symlinks aligned if uboot-tools placed binaries there
    rm -f "$TARGET_DIR/usr/sbin/fw_printenv" "$TARGET_DIR/usr/sbin/fw_setenv"
    ln -sf ../../sbin/fw_printenv "$TARGET_DIR/usr/sbin/fw_printenv"
    ln -sf ../../sbin/fw_printenv "$TARGET_DIR/usr/sbin/fw_setenv"
fi

# 6. Install trimmed G3 Kernel, DTB, and kernel modules (plus external SmartGateway BSP .ko)
if [ -f "$K3_OUT/zImage" ] && [ -f "$K3_OUT/100ask_imx6ull-14x14.dtb" ]; then
    cp -f "$K3_OUT/zImage" "$TARGET_DIR/boot/zImage"
    cp -f "$K3_OUT/100ask_imx6ull-14x14.dtb" "$TARGET_DIR/boot/100ask_imx6ull-14x14.dtb"
fi
if [ -d "$K3_MODS/lib/modules" ]; then
    rm -rf "$TARGET_DIR/lib/modules"
    cp -a "$K3_MODS/lib/modules" "$TARGET_DIR/lib/modules"
    # Remove broken source/build symlinks inside target rootfs
    find "$TARGET_DIR/lib/modules" -maxdepth 2 \( -name build -o -name source \) -exec rm -rf {} +
    kver_dir="$(find "$TARGET_DIR/lib/modules" -mindepth 1 -maxdepth 1 -type d | head -n 1)"
    if [ -n "$kver_dir" ] && [ -d "$K3_BSP_KO" ]; then
        mkdir -p "$kver_dir/extra"
        cp -f "$K3_BSP_KO"/*.ko "$kver_dir/extra/" 2>/dev/null || true
    fi
fi
