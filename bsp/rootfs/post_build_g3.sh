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
    cp -f "$BR_OVERLAY/etc/sw-versions" "$TARGET_DIR/etc/sw-versions"

    chmod 755 "$TARGET_DIR/etc/init.d/S99adbd" "$TARGET_DIR/etc/init.d/S50sshd" "$TARGET_DIR/usr/bin/usb_config"
fi

# Install SmartGateway G4 SWUpdate board identity, config, and CLI launcher
printf "imx6ull-100ask revA\n" > "$TARGET_DIR/etc/hwrevision"
chmod 644 "$TARGET_DIR/etc/hwrevision"
rm -f "$TARGET_DIR/etc/swupdate/postupdate.sh" "$TARGET_DIR/etc/swupdate/swupdate-public.pem"
cp -f "$REPO_DIR/bsp/ota/swupdate.cfg" "$TARGET_DIR/etc/swupdate/swupdate.cfg"
chmod 644 "$TARGET_DIR/etc/swupdate/swupdate.cfg"
cp -f "$REPO_DIR/bsp/ota/sgw-swu-install.sh" "$TARGET_DIR/usr/bin/sgw-swu-install"
chmod 755 "$TARGET_DIR/usr/bin/sgw-swu-install"

# 3. Corrected /etc/fstab (ext4 root + persistent /opt/smartgateway/data partition mmcblk1p3)
cat > "$TARGET_DIR/etc/fstab" <<'FSEOF'
# <file system>	<mount pt>	<type>	<options>	<dump>	<pass>
/dev/root	/		ext4	rw,noauto	0	1
proc		/proc		proc	defaults	0	0
devpts		/dev/pts	devpts	defaults,gid=5,mode=620,ptmxmode=0666	0	0
tmpfs		/dev/shm	tmpfs	mode=0777	0	0
tmpfs		/tmp		tmpfs	mode=1777	0	0
tmpfs		/run		tmpfs	mode=0755,nosuid,nodev	0	0
sysfs		/sys		sysfs	defaults	0	0
/dev/mmcblk1p3	/opt/smartgateway/data	ext4	defaults	0	2
FSEOF

# 4. Board-verified SysV rcS/rcK (without psplash) & clean /etc/profile
#    Defer S99smartgateway until after rcS returns so BusyBox PID 1 enters RESPAWN,
#    and avoid stripping ::respawn: from /etc/inittab during rcK reboot/shutdown.
cat > "$TARGET_DIR/etc/init.d/rcS" <<'RCEOF'
#!/bin/sh
for i in /etc/init.d/S??* ; do
    [ ! -f "$i" ] && continue
    case "$i" in
        *S99smartgateway)
            ;;
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
if [ -x /etc/init.d/S99smartgateway ]; then
    (sleep 2; /etc/init.d/S99smartgateway start) </dev/null >/dev/console 2>&1 &
fi
RCEOF
chmod 755 "$TARGET_DIR/etc/init.d/rcS"

cat > "$TARGET_DIR/etc/init.d/rcK" <<'RKEOF'
#!/bin/sh
for i in $(ls -r /etc/init.d/S??*) ; do
    [ ! -f "$i" ] && continue
    case "$i" in
        *S99smartgateway)
            /opt/smartgateway/bin/service_supervisor stop --root /opt/smartgateway >/dev/null 2>&1 || true
            ;;
        *.sh)
            (
                trap - INT QUIT TSTP
                set stop
                . "$i"
            )
            ;;
        *)
            "$i" stop
            ;;
    esac
done
RKEOF
chmod 755 "$TARGET_DIR/etc/init.d/rcK"

cat > "$TARGET_DIR/etc/profile" <<'PREOF'
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
PREOF

# 5. Redundant U-Boot env config + A/B tools + Service Supervisor + S12/S15/S99 init scripts
cp -f "$REPO_DIR/bsp/uboot/fw_env.ab.config" "$TARGET_DIR/etc/fw_env.config"
chmod 644 "$TARGET_DIR/etc/fw_env.config"
cp -f "$REPO_DIR/bsp/apps/boot_ab/S15sgw_ab_guard" "$TARGET_DIR/etc/init.d/S15sgw_ab_guard"
chmod 755 "$TARGET_DIR/etc/init.d/S15sgw_ab_guard"

cat > "$TARGET_DIR/etc/init.d/S12sgw_drivers" <<'DREOF'
#!/bin/sh
case "$1" in
    start)
        for m in /lib/modules/*/extra/sgw_ap3216.ko /lib/modules/*/extra/sgw_gpio_event.ko; do
            [ -f "$m" ] && insmod "$m" 2>/dev/null || true
        done
        ;;
    stop)
        rmmod sgw_gpio_event 2>/dev/null || true
        rmmod sgw_ap3216 2>/dev/null || true
        ;;
    *)
        exit 0
        ;;
esac
DREOF
chmod 755 "$TARGET_DIR/etc/init.d/S12sgw_drivers"

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
    rm -f "$TARGET_DIR/usr/sbin/fw_printenv" "$TARGET_DIR/usr/sbin/fw_setenv"
    ln -sf ../../sbin/fw_printenv "$TARGET_DIR/usr/sbin/fw_printenv"
    ln -sf ../../sbin/fw_printenv "$TARGET_DIR/usr/sbin/fw_setenv"
fi

# Pre-populate SysV/BusyBox inittab respawn entry once so signed /etc/inittab hash never mutates
# Keep process field <= 127 chars for SysV init while preserving SMARTGATEWAY_RESPAWN=1 and service_supervisor match.
if [ -f "$TARGET_DIR/etc/inittab" ] && ! grep -q '^sgw0::respawn:.*service_supervisor' "$TARGET_DIR/etc/inittab"; then
    sed -i '/^# G4b:/d; /^::respawn:.*service_supervisor/d' "$TARGET_DIR/etc/inittab"
    cat >> "$TARGET_DIR/etc/inittab" <<'INEOF'

# G4b: sysvinit respawns service_supervisor automatically (unique id + <= 127-char process field).
sgw0::respawn:/bin/sh -c 'SMARTGATEWAY_RESPAWN=1 exec /opt/smartgateway/bin/service_supervisor_run'
INEOF
fi

# 6. Install SmartGateway runtime skeleton & strict-default network/policy dir
install -d -m 0755 "$TARGET_DIR/opt"
install -d -m 0700 "$TARGET_DIR/opt/smartgateway" \
    "$TARGET_DIR/opt/smartgateway/bin" \
    "$TARGET_DIR/opt/smartgateway/data" \
    "$TARGET_DIR/opt/smartgateway/logs" \
    "$TARGET_DIR/opt/smartgateway/packages" \
    "$TARGET_DIR/opt/smartgateway/releases" \
    "$TARGET_DIR/opt/smartgateway/releases/0.6.0" \
    "$TARGET_DIR/opt/smartgateway/run" \
    "$TARGET_DIR/opt/smartgateway/state"
: > "$TARGET_DIR/opt/smartgateway/.ota.lock"
chmod 600 "$TARGET_DIR/opt/smartgateway/.ota.lock"
cat > "$TARGET_DIR/opt/smartgateway/state/ota_state.json" <<'OTEOF'
{"schema_version":1,"state":"CONFIRMED","confirmed_version":"0.6.0","candidate_version":"0.6.0"}
OTEOF
chmod 600 "$TARGET_DIR/opt/smartgateway/state/ota_state.json"
cat > "$TARGET_DIR/opt/smartgateway/service.conf" <<'SCEOF'
executable=/opt/smartgateway/data/app/current/smart_gateway
serial=
baud=115200
server=127.0.0.1
port=9000
stop_timeout_ms=5000
SCEOF
chmod 600 "$TARGET_DIR/opt/smartgateway/service.conf"
if [ -x "$REPO_DIR/build/arm-s5/service_supervisor" ]; then
    install -m 0700 "$REPO_DIR/build/arm-s5/service_supervisor" "$TARGET_DIR/opt/smartgateway/bin/service_supervisor"
fi
install -m 0700 "$REPO_DIR/bsp/apps/service_supervisor/service_supervisor_run" "$TARGET_DIR/opt/smartgateway/bin/service_supervisor_run"
if [ -x "$REPO_DIR/build/arm-d25/ota_agent" ]; then
    install -m 0700 "$REPO_DIR/build/arm-d25/ota_agent" "$TARGET_DIR/opt/smartgateway/bin/ota_agent"
fi
if [ -x "$REPO_DIR/build/arm-d25/smart_gateway" ]; then
    install -m 0700 "$REPO_DIR/build/arm-d25/smart_gateway" "$TARGET_DIR/opt/smartgateway/releases/0.6.0/smart_gateway"
fi
ln -sfn /opt/smartgateway/data/app/current "$TARGET_DIR/opt/smartgateway/current"

install -d -m 0700 "$TARGET_DIR/etc/smartgateway"
printf "interface=eth0\n" > "$TARGET_DIR/etc/smartgateway/system-health-network.conf"
chmod 600 "$TARGET_DIR/etc/smartgateway/system-health-network.conf"
if [ -n "${SGW_OTA_PUBKEY_FILE:-}" ] && [ -f "$SGW_OTA_PUBKEY_FILE" ]; then
    install -m 0600 "$SGW_OTA_PUBKEY_FILE" "$TARGET_DIR/etc/smartgateway/ota-public.pem"
fi
if [ -n "${SGW_R3C_POLICY_FILE:-}" ] && [ -f "$SGW_R3C_POLICY_FILE" ]; then
    install -m 0600 "$SGW_R3C_POLICY_FILE" "$TARGET_DIR/etc/smartgateway/r3c-policy.conf"
fi
if [ -n "${SGW_ETH0_STATIC_IP:-}" ]; then
    cat > "$TARGET_DIR/etc/network/interfaces" <<IFEOF
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet static
    address ${SGW_ETH0_STATIC_IP}
    netmask ${SGW_ETH0_NETMASK:-255.255.255.0}
IFEOF
    chmod 644 "$TARGET_DIR/etc/network/interfaces"
fi

# 7. Install trimmed G3 Kernel, DTB, and kernel modules (plus external SmartGateway BSP .ko)
if [ -f "$K3_OUT/zImage" ] && [ -f "$K3_OUT/100ask_imx6ull-14x14.dtb" ]; then
    cp -f "$K3_OUT/zImage" "$TARGET_DIR/boot/zImage"
    cp -f "$K3_OUT/100ask_imx6ull-14x14.dtb" "$TARGET_DIR/boot/100ask_imx6ull-14x14.dtb"
fi
if [ -d "$K3_MODS/lib/modules" ]; then
    rm -rf "$TARGET_DIR/lib/modules"
    cp -a "$K3_MODS/lib/modules" "$TARGET_DIR/lib/modules"
    find "$TARGET_DIR/lib/modules" -maxdepth 2 \( -name build -o -name source \) -exec rm -rf {} +
    kver_dir="$(find "$TARGET_DIR/lib/modules" -mindepth 1 -maxdepth 1 -type d | head -n 1)"
    if [ -n "$kver_dir" ] && [ -d "$K3_BSP_KO" ]; then
        mkdir -p "$kver_dir/extra"
        cp -f "$K3_BSP_KO"/*.ko "$kver_dir/extra/" 2>/dev/null || true
    fi
fi
