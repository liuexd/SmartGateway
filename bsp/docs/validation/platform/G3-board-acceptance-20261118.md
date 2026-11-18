# G3 Trimmed Kernel & Buildroot RootFS — 100ASK i.MX6ULL Board Acceptance Report (2026-11-18)

## 1. Acceptance Scope & Result

- **Hardware Target:** `100ASK_IMX6ULL` (`mmcblk1`: A/B system slots `p1`/`p2` + persistent data partition `p3` + redundant U-Boot env `0xE0000`/`0xE2000`).
- **Deployment Slot:** Candidate slot `/dev/mmcblk1p1` (A/B trial -> `CONFIRMED`); factory golden slot `/dev/mmcblk1p2` kept completely untouched (`mount -o ro` verified).
- **Acceptance Status:** **PASS** (within documented policy and sensor-heartbeat boundaries below).

## 2. Board-Discovered Integration Fixes & Build Hardening

First-boot testing of the trimmed G3 rootfs (`build_rootfs_g3.sh`) exposed three real target/host integration issues that did not fail in earlier offline/simulated checks:

1. **Root Ownership (`1000:1000` -> `0:0`) & `/var/empty` Permissions (`bsp/rootfs/build_rootfs_g3.sh`):**
   - **Symptom on Board:** `mount: you must be root`, `/proc` & `/sys` failed to mount, `/var/empty must be owned by root`, `S99adbd` failed with `ls: /sys/class/udc/: No such file or directory`.
   - **Root Cause:** Buildroot 2020.02 `host-fakeroot` (v1.24) did not properly intercept `chown`/`statx` on Ubuntu 24.04 (`glibc 2.39`), producing `1000:1000` ownership across `rootfs.tar`/`rootfs.ext4` (including setuid `4755` `/bin/busybox`), while `mkusers` also set `/var/empty` to `1000:1003` (`sshd`).
   - **Fix:** After building `host-fakeroot`, shim `build/buildroot-g3/host/bin/fakeroot` to host `/usr/bin/fakeroot`, add `BR2_ROOTFS_POST_FAKEROOT_SCRIPT` (`chown -h -R 0:0 "$1/var/empty"` + `chmod 0755 "$1/var/empty"`), and assert `uid==0, gid==0` on critical rootfs entries in both `build_rootfs_g3.sh` and `tests/test_g3_kernel_rootfs.py`.
2. **Missing `libbsd.so.0` Runtime Library (`bsp/rootfs/build_rootfs_g3.sh`):**
   - **Symptom on Board:** `ip: error while loading shared libraries: libbsd.so.0: cannot open shared object file` during `S40network`.
   - **Root Cause:** The external SDK sysroot includes `libbsd.so.0.10.0`, causing `iproute2` (`/sbin/ip`) to dynamically link against `libbsd.so.0`, while `g3_trimmed_defconfig` omitted `BR2_PACKAGE_LIBBSD`.
   - **Fix:** Enable `BR2_PACKAGE_LIBBSD=y` in `build_rootfs_g3.sh`'s generated `TMP_DEF` and verify `usr/lib/libbsd.so.0` in `rootfs.tar` and `tests/test_g3_kernel_rootfs.py`.
3. **SysV `init` `/etc/inittab` Process Length & Non-Empty ID (`bsp/rootfs/post_build_g3.sh`, `bsp/apps/service_supervisor/S99smartgateway`):**
   - **Symptom on Board:** `INIT: /etc/inittab[34]: process field too long (max 127 characters)` and subsequently `INIT: /etc/inittab[34]: missing id field` (G3 uses `BR2_INIT_SYSV=y`, whereas factory `p2` ran BusyBox `init` which lacks these restrictions).
   - **Fix:** Install `/opt/smartgateway/bin/service_supervisor_run` (`0700` wrapper redirecting stdout/stderr to `/opt/smartgateway/logs/supervisor.log`) and register `sgw0::respawn:/bin/sh -c 'SMARTGATEWAY_RESPAWN=1 exec /opt/smartgateway/bin/service_supervisor_run'` (unique ID `sgw0`, 84-char process field `<= 127`, preserving `SMARTGATEWAY_RESPAWN=1` and `service_supervisor` match). Updated `S99smartgateway` to emit `sgw0::respawn:` when `/etc/inittab` uses `:initdefault:` (SysV `init`) and `::respawn:` on BusyBox `init`.

## 3. Verified Artifacts & Sizes

- **Trimmed G3 Kernel & DTB (`build/kernel-g3/trimmed/out/`):**
  - `zImage`: `4,848,520` B (SHA-256 `dfb2b647a5e650285bc0025e3892617481595caf66d8868d948bc10d438eca60`) vs factory `p2` `zImage` `8,873,376` B.
  - `100ask_imx6ull-14x14.dtb`: `38,707` B (SHA-256 `348f96f323376fe4587231a4bd78ac8888df1075cab8c9dba33d55fb6e6aa4cb`), includes `ap3216@1e`, `sgw-gpio-event` (`GPIO4_IO19`), and `32 MiB` CMA pool.
- **Trimmed G3 RootFS (`build/buildroot-g3/` & `g3_rootfs_metrics.json`):**
  - `rootfs.ext4`: `134,217,728` B (`128 MiB`, `-O ^metadata_csum`), post-provision `p1` SHA-256 `303db34cd3b19744e44573b1b9453772369671bdd25723b0e515f2c7d2955ea1`, `e2fsck -fn`: `1315/32768` files, `42068/131072` blocks.
  - `rootfs.tar`: `34,816,000` B archive / `32,683,661` B payload (`1,303` entries); `154` resolved `BR2_PACKAGE_*` (`315` in baseline core defconfig).
  - Signed 15-file manifest SHA-256: `0338557d7575c36b36c98d30013a9632a782f61a1d5b1de1333f07b0ab176e6e`.
  - Confirmed TXN (`p3/.sgw-tx/txn-2c9afe469b3aadc5844315723750715b9c8f30a252e961dbe05072dbfd4a749e`): `state=CONFIRMED`, `target=1`, `last_good=2`, `written_size=134217728`, `consumed=1`, `bootcount=1`.

## 4. Board Hardware Acceptance Evidence

Observed via COM3 UART (`g3_uart_capture.log`, 2,907 lines, MACs/EUI-64 redacted) and live `adb shell` (`g3_board_verify.log`):

1. **Automatic Failed-Upgrade Rollback (Observed Live During First Broken Image Boot):**
   - Initial G3 attempt hit `Bootlimit (3) exceeded` across 3 `WDOG` resets and automatically ran `altbootcmd` -> `[AB-OTA] Bootlimit exceeded, rolling back to slot 2` -> booted untouched factory `p2` cleanly.
2. **G3 Candidate Slot (`p1`) Boot, Memory & Partition Layout:**
   - U-Boot loaded `zImage` (`4848520` B) and DTB (`38707` B); kernel booted `console=ttymxc0,115200 root=/dev/mmcblk1p1 rootwait rw panic=5`.
   - `32 MiB` CMA pool active (`475836K/524288K` kernel available); userspace `free -m`: `497` total / `477` available MiB (vs factory `p2` `320 MiB` CMA / `~172 MiB` available).
   - `/dev/mmcblk1p1` mounted rw on `/` (`fstab` + `devtmpfs`/`proc`/`sysfs`/`devpts`/`configfs`/`functionfs`), `/dev/mmcblk1p3` mounted rw on `/opt/smartgateway/data`.
3. **Dual BSP Sensor Drivers (`S12sgw_drivers`):**
   - `sgw_ap3216` (`0-001e`): `als_raw=0`, `stats`: `read_success=3`, `read_errors=0`.
   - `sgw_gpio_event`: `/dev/sgw_event0` ready (`IRQ 194`, `debounce=20ms`, `queue capacity=64`).
4. **Network, SSH, ADB, and `gdbserver`:**
   - `eth0` `<BROADCAST,MULTICAST,UP,LOWER_UP>` provisioned `192.168.10.3/24`; `ping -c 3 192.168.10.3` from Ubuntu VM: `3/3` packets received (`0%` loss).
   - `sshd` listening on `0.0.0.0:22` & `[::]:22`; `adbd` running over USB Gadget `ci_hdrc.0`; `gdbserver --version`: `GNU gdbserver (GDB) 8.2.1`.
5. **Supervisor / Gateway Runtime, `S99smartgateway`, G4b Respawn & Steady-State A/B Confirmation:**
   - First `trial` boot auto-confirmed: `sgw_bootctl status` reports `valid=1`, `sgw_active_slot=1`, `sgw_last_good_slot=1`, `sgw_pending_slot=none`, `sgw_ab_state=confirmed`, `upgrade_available=0`, `bootcount=0`; TXN moved to `state=CONFIRMED`, `consumed=1`.
   - Negative confirmation test: `sgw_bootctl confirm --running 1` in confirmed state rejected with `CONFIRM REJECTED: no active unconsumed transaction record` (`rc=1`).
   - Lifecycle test: `S99smartgateway stop` -> `status` `stopped` (`rc=1`) -> `S99smartgateway start` -> re-registered `sgw0::respawn:` -> `supervisor` `ppid=1` & `smart_gateway` running (`7000/tcp`).
   - G4b crash self-healing test: `kill -9` on `service_supervisor` PID `258` -> SysV `init` (`PID 1`) automatically respawned new `service_supervisor` PID `480` (`ppid=1`), which re-adopted running `smart_gateway` PID `272`.
   - Steady-state `reboot` on confirmed `p1` returned cleanly to `slot=1`, `sgw_ab_state=confirmed`, `bootcount=0` with `service_supervisor`, `smart_gateway`, `sgw_ap3216`, `sgw_gpio_event`, `eth0`, `sshd`, and `adbd` operational.
   - Untouched factory `p2` check: `mount -o ro /dev/mmcblk1p2` confirmed stock `p2` `/etc/issue`, `8873376` B `zImage`, and `5408` B `/sbin/init`.

## 5. Host Regression Tests (Post-Final-Edit)

Re-ran full G1/G2/G3 + Review4 Python suites on Ubuntu VM after final edits (`SGW_TEST_FUSE2FS=/usr/bin/fuse2fs`):
- `python3 -m unittest discover -s tests -p 'test_g*.py' -v`: **82/82 OK** (`37.373s`)
- `python3 -m unittest tests/test_review4_g3_bundle.py -v`: **1/1 OK** (`0.501s`)
- Combined archived output: `bsp/docs/validation/platform/g3-20261118-logs/g3_full_python.log` (**83/83 OK**).

## 6. Boundaries & Exclusions

- On-board `p1` R3C policy was provisioned with `gateway_flow_mode=controlled_local`, `allow_external_degraded=1`, and `require_sensor_heartbeat=0` because no external northbound broker or live NODE04 frame transmitter was attached during G3 image acceptance; this does **not** certify `strict_forward` or `require_sensor_heartbeat=1` policies without live northbound + sensor links.
- Kernel G3 build (`build/kernel-g3/`) was preserved from clean-directory build (`f0ecb41`); pre-existing uncommitted files (`bsp/kernel/g3_kernel_config.diff`, `bsp/kernel/g3_trimmed.fragment`, `bsp/rootfs/g3_trimmed_defconfig`) and untracked `bsp/docs/project-roadmap/` remain outside git commits.

## 7. Archived Evidence Files

- `bsp/docs/validation/platform/g3-20261118-board-logs/g3_uart_capture.log` (full boot/rollback/confirm UART trace, MACs/EUI-64 redacted)
- `bsp/docs/validation/platform/g3-20261118-board-logs/g3_board_verify.log`
- `bsp/docs/validation/platform/g3-20261118-board-logs/g3_confirmed_txn.txt`
- `bsp/docs/validation/platform/g3-20261118-board-logs/g3_rootfs.manifest`
- `bsp/docs/validation/platform/g3-20261118-logs/g3_rootfs_metrics.json`
- `bsp/docs/validation/platform/g3-20261118-logs/g3_full_python.log`
