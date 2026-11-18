# G2 Physical Board Acceptance Report (i.MX6ULL, 2026-11-18)

**Status:** `BOARD-ACCEPTED` (physical `100ASK_IMX6ULL` board verification of A/B U-Boot `2017.03`, redundant `0xE0000`/`0xE2000` MMC environment, `WDOG1` hardware reset & `bootlimit=3` auto-rollback to untouched `p2`, and `p1` production `sgw_watchdog_guard` + `S15sgw_ab_guard` confirmation against `mmcblk1p3` ext4 DATA, signed 15-file manifest, `service_supervisor`, `smart_gateway`, `sgw_ap3216`, `sgw_gpio_event`, and `eth0`).

## 1. Deployment & Flash Verification

1. **Pre-flash Backup & Redundant Env Migration:**
   - Live single-copy `0xE0000` pulled from `/dev/mmcblk1` (`sha256: 5ef954c13771dc91e8fd85fd6df5209e882b4f8973c6c0337304f3e4ea7ba4f3`), migrated via `bsp/uboot/migrate_single_to_redund_env.sh` preserving `ethaddr=[REDACTED]`, `eth1addr=[REDACTED]`, `panel=TFT7016`, `fdt_file=100ask_imx6ull-14x14.dtb`, `update=yes`.
   - Both redundant copies (`0xE0000` and `0xE2000`) verified CRC32 (`0xccc9ea90` / `0x8037b34a`, flags `100` / `99`), written at sector `1792` (`16384` bytes), readback SHA-256 matched `29b6bf16c7372585302a285af23d820d10f76a0c2c485eb2b680851daf5df715`.
2. **A/B U-Boot (`u-boot-dtb.imx`) on `/dev/mmcblk1boot0`:**
   - Built from `build/uboot-ab/arm/u-boot-dtb.imx` (`527360` bytes), written to `/dev/mmcblk1boot0` (`bs=1024 seek=1`) with `force_ro` re-enabled (`1`), readback SHA-256 matched `b741e791a82db4646f81fb28e7bd3107310ef5079054c017cf6f2730dacec4e0`.
   - Initial steady boot verified on `COM3` UART: `U-Boot 2017.03-g00d6e27-dirty (Nov 18 2026 - 11:58:24 +0800)`, `Watchdog enabled`, `[AB-OTA] boot slot=2 state=confirmed bootcount=1/3 upgrade_available=0`, kernel command line `console=ttymxc0,115200 root=/dev/mmcblk1p2 rootwait rw panic=5`.
3. **Test Slot (`p1`) & Persistent Data Partition (`p3`):**
   - `/dev/mmcblk1p2` root filesystem remained untouched (verified no `/etc/smartgateway`, no `S15sgw_ab_guard`, no `sgw_bootctl`, original single-copy `/etc/fw_env.config`).
   - `/dev/mmcblk1p3` (`500 MiB`, previously zeroed/unformatted) formatted `ext4` (`sgw_data`), mounted at `/opt/smartgateway/data` via `/etc/fstab` (`0700` `root:root`), initialized `.sgw-data-schema` (`schema=1\n`) and `.sgw-tx/` (`0700`).
   - `/dev/mmcblk1p1` populated with `fw_printenv`/`fw_setenv` (`0xE0000`+`0xE2000` `/etc/fw_env.config`), `sgw_bootctl`, `sgw_watchdog_guard`, `sgw_system_health`, `service_supervisor`, `smart_gateway`, `ota_agent`, fixed `S15sgw_ab_guard` (commit `f06c12a`), `S12sgw_drivers` (`sgw_ap3216.ko`, `sgw_gpio_event.ko`), merged DTB containing both `ap3216@1e` and `sgw-gpio-event`, `/etc/smartgateway/{ota-public.pem,r3c-policy.conf,system-health-network.conf}`, and `p1` `rcS`/`rcK` adjustments so `service_supervisor` starts cleanly under BusyBox `init` `RESPAWN` without mutating signed `/etc/inittab` on reboot.

## 2. Board Verification Results

1. **Scenario 1 — Failed Trial -> `WDOG1` Hardware Reset x3 -> `bootlimit=3` Auto-Rollback to Untouched `p2` (PASS):**
   - Staged `sgw_active_slot=1`, `sgw_pending_slot=1`, `sgw_last_good_slot=2`, `sgw_ab_state=trial`, `upgrade_available=1`, `bootcount=0` with empty `/opt/smartgateway/data/.sgw-tx/`.
   - Observed 3 consecutive failed trial boots in `COM3` UART log ([g2_uart_capture_20261118.log](g2-board-logs/g2_uart_capture_20261118.log)):
     - `bootcount=1/3` -> `Starting sgw_watchdog_guard...` -> `[   54.124869] watchdog: watchdog0: watchdog did not stop!` -> `Reset cause: WDOG`
     - `bootcount=2/3` -> `[   54.144721] watchdog: watchdog0: watchdog did not stop!` -> `Reset cause: WDOG`
     - `bootcount=3/3` -> `[   54.324578] watchdog: watchdog0: watchdog did not stop!` -> `Reset cause: WDOG`
   - On 4th boot (`bootcount=4 > bootlimit=3`): U-Boot emitted `Warning: Bootlimit (3) exceeded. Using altbootcmd.` -> `[AB-OTA] Bootlimit exceeded, rolling back to slot 2` -> `[AB-OTA] boot slot=2 state=rolled_back bootcount=0/3 upgrade_available=0` and booted untouched `/dev/mmcblk1p2` (`Kernel command line: console=ttymxc0,115200 root=/dev/mmcblk1p2 rootwait rw panic=5`).
   - Verified flash env via `fw_printenv`: `sgw_ab_state=rolled_back`, `sgw_active_slot=2`, `sgw_last_good_slot=2`, `sgw_pending_slot=none`, `sgw_last_error=bootlimit_exceeded`, `sgw_rollback_count=1`, `upgrade_available=0`, `bootcount=0`.

2. **Scenario 2 — Signed `p1` Trial Boot -> Live R3C Watchdog Guard Confirmation -> Steady `slot=1` (PASS):**
   - Staged `PENDING` transaction `22d07be87babd0ee64930e1b5671c4888f46652fc8a67d6b641f31f537bc4a1e` with 15-file manifest ([p1_rootfs.manifest](g2-board-logs/p1_rootfs.manifest), manifest SHA-256 `88a8512ba1dee7e0b7e0a29224d42495674eb9b32e7b64ee760ac54ba68c477a`) under `/etc/smartgateway/r3c-policy.conf` (`gateway_flow_mode=controlled_local`, `allow_external_degraded=1`, `require_sensor_heartbeat=0`, `stable_ms=3000`, `deadline_ms=25000`) and staged `trial -> slot 1`.
   - Observed `p1` boot cleanly:
     - U-Boot: `[AB-OTA] boot slot=1 state=trial bootcount=1/3 upgrade_available=1`
     - Drivers probed: `sgw_ap3216 0-001e: probe success: old=0x03 current=0x03`, `sgw-gpio-event sgw-gpio-event: misc device /dev/sgw_event0 ready` (`IRQ 194 ready, queue capacity=64`)
     - Supervisor & Gateway active under `init` (`supervisor: running pid=918 ppid=1`, `gateway: status: running pid=976`)
     - `sgw_watchdog_guard` confirmed trial boot and closed `/dev/watchdog` cleanly with `'V'` (`up 0:10`, zero watchdog resets).
   - Verified post-confirm state ([g2_board_p1_verify.log](g2-board-logs/g2_board_p1_verify.log)):
     - Transaction `txn-22d07be87...` updated to `state=CONFIRMED`, `consumed=1`, `boot_id=618d4fb7-a366-43ae-aaa8-63efd5648051`, `bootcount=1`.
     - `sgw_bootctl status`: `valid=1`, `sgw_active_slot=1`, `sgw_last_good_slot=1`, `sgw_pending_slot=none`, `sgw_ab_state=confirmed`, `upgrade_available=0`, `bootcount=0`, `sgw_last_error=none`.
     - Negative fail-closed check on steady `p1`: `sgw_bootctl confirm` rejected with `CONFIRM REJECTED: no active unconsumed transaction record` (`exit=1`).

## 3. Notes & Next Phase Readiness

- **Scope Boundary:** Physical board `p1` confirmation exercised `gateway_flow_mode=controlled_local`, `allow_external_degraded=1`, and `require_sensor_heartbeat=0` (with direct `ap3216@1e` sysfs + `sgw_gpio_event` drivers healthy); this does not certify `strict_forward` northbound forwarding or `require_sensor_heartbeat=1` (NODE04 heartbeat) without explicit northbound/sensor integration.
- Development board is currently running on confirmed `slot=1` (`/dev/mmcblk1p1`, `up >10 min`) with `/dev/mmcblk1p2` preserved untouched.
- Repository commit `f06c12a` (`bsp/apps/boot_ab/S15sgw_ab_guard`) fixed `-c /etc/fw_env.config` override trigger; note `p1` validation also required adjusting `p1`'s `/etc/init.d/rcS` & `/etc/init.d/rcK` ordering (so `S99smartgateway` runs after `rcS` finishes `S99wificonnect` and does not strip `::respawn:` from `/etc/inittab` on reboot), which should be incorporated into Phase 3 (`bsp/rootfs/post_build_g3.sh`) when building the trimmed G3 rootfs.
