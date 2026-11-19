# G3 `/etc/inittab` 15/15 Manifest Stability Recheck & G4 Isolated `.swu` Report (2026-11-18)

## 1. Objective

Resolve the runtime SHA-256 drift on `/etc/inittab` (previously 14/15 manifest match after `S99smartgateway stop/start`), clean-rebuild G3 from scratch, re-verify on physical `100ASK_IMX6ULL` slot `/dev/mmcblk1p1` with **15/15** manifest match across every lifecycle transition (leaving old archived manifests untouched), and verify G4 `.swu` packaging, signature/payload/slot checks, write/readback, and trial staging on isolated virtual disks (leaving physical `/dev/mmcblk1p2` untouched).

## 2. G3 `/etc/inittab` Stability Fix

- **`bsp/apps/service_supervisor/S99smartgateway`:** Removed `ensure_respawn_entry()` and `remove_respawn_entry()`; `S99smartgateway` no longer modifies `/etc/inittab`. `start` performs a read-only check for `^[^#]*:respawn:.*service_supervisor` (failing closed if missing), clears runtime stop marker `$STOP_FLAG` (`/run/smartgateway.disabled` on board `tmpfs`, or `$RUN_DIR/supervisor.disabled` / `SMARTGATEWAY_STOP_FLAG` in isolated tests), and waits for `service_supervisor` via `init`. `stop` creates `$STOP_FLAG` before stopping `service_supervisor` and `smart_gateway`.
- **`bsp/apps/service_supervisor/service_supervisor_run` & `bsp/rootfs/post_build_g3.sh`:** Extracted a single shared wrapper `bsp/apps/service_supervisor/service_supervisor_run` (waits on `$STOP_FLAG` with `trap 'exit 0' TERM INT; while [ -f "$STOP_FLAG" ]; do sleep 1; done` before `exec "$ROOT/bin/service_supervisor" run --root "$ROOT"`) installed identically by `post_build_g3.sh` (`0700`) and standalone `install.sh`. Pinned `sgw0::respawn:/bin/sh -c 'SMARTGATEWAY_RESPAWN=1 exec /opt/smartgateway/bin/service_supervisor_run'` in `/etc/inittab`.
- **`bsp/apps/service_supervisor/install.sh`:** Unified standalone installer with image build and moved signed/pre-pinned image guard ahead of **all** filesystem writes: on signed (`r3c-policy.conf` / `ota-public.pem` under `POLICY_DIR`) or pre-pinned (`service_supervisor_run` in `inittab`) images, verifies exact byte-for-byte identity of `inittab` entry, `S99smartgateway`, and `service_supervisor_run` before touching disk—skipping file installation (`no .bak.*`, identical inode/digest) when matched and failing closed with zero mutation to `inittab`, `S99smartgateway`, or `service_supervisor_run` on any version mismatch. Fresh standalone installations continue deploying normally without exceeding SysV `init`'s 127-char limit.
- **`tests/test_g3_inittab_digest_stability.py` & `tests/test_s5r2_stop_respawn.py`:** Added 5 automated tests covering image-mode lifecycle digest stability, standalone `install.sh` followed by `stop -> 2.5s hold -> start -> SIGKILL crash respawn`, pre-pinned signed-image `install.sh` skip (zero mutation on `inittab`, `S99smartgateway`, `service_supervisor_run`), signed/pre-pinned version mismatch rejection (mismatched `S99smartgateway`, `inittab`, and `service_supervisor_run` with zero digest change), and missing-entry fail-closed check.

## 3. Clean-Directory Rebuild & Host Regression (Post-Final-Edit)

- Clean rebuild (`rm -rf build/kernel-g3 build/buildroot-g3`, `./bsp/kernel/build_kernel_g3.sh`, `./bsp/rootfs/build_rootfs_g3.sh`):
  - Trimmed `zImage`: `4,848,520` B; `100ask_imx6ull-14x14.dtb`: `38,707` B (`32 MiB` CMA, `ap3216_in_dtb=true`, `gpio_in_dtb=true`, `48` modules).
  - Trimmed `rootfs.ext4`: `134,217,728` B (`128 MiB`, `-O ^metadata_csum`); `rootfs.tar`: `34,816,000` B archive / `32,679,384` B payload (`1,303` entries, `154` resolved `BR2_PACKAGE_*` vs `315` baseline).
- Host regression (`SGW_TEST_FUSE2FS=/usr/bin/fuse2fs`):
  - `python3 -m unittest discover -s tests -p 'test_g*.py' -v`: **92/92 OK** (`83.424s`, includes 5/5 `test_g3_inittab_digest_stability` and 5/5 `test_g4_swu_isolated`)
  - `python3 -m unittest tests/test_review4_g3_bundle.py -v`: **1/1 OK** (`0.527s`)
  - Combined archive: `bsp/docs/validation/platform/g3-20261118-recheck-logs/g3_g4_host_python.log` (**93/93 OK**).

## 4. Physical Board `p1` Recheck Evidence (15/15 Manifest Match)

Flashed clean-rebuilt G3 `rootfs.ext4` (`134,217,728` B) to `/dev/mmcblk1p1` from temporary golden `p2` session, provisioned `/etc/network/interfaces`, `/etc/smartgateway/ota-public.pem`, and `/etc/smartgateway/r3c-policy.conf` (`controlled_local`, `allow_external_degraded=1`, `require_sensor_heartbeat=0`), yielding post-provision `IMG_SHA=ab24a7c4c656b0b39f9a1e312b1ae276fa52dda30883b7034f0043a4cf6678a0`, `MANIFEST_SHA=018181481873274daae2d66dce4c381fe2bb2393a285be7cfda4fbff69debb03`, and `TXN_ID=acc7c48c1a9a1e134948ecfd9ad3b03b81e0b484b2f9acfe47b62e5fbada2936`:

1. **Confirmed Boot (`confirmed_boot`):** `root=/dev/mmcblk1p1`, `sgw_ab_state=confirmed`, `sgw_active_slot=1`, `sgw_last_good_slot=1`, `bootcount=0`, `upgrade_available=0`, TXN `state=CONFIRMED`, `consumed=1`, `bootcount=1`; `supervisor: running (pid=347 ppid=1)`, `gateway: running (pid=393)`, manifest **15/15 MATCH**.
2. **`S99smartgateway stop` + 5s Hold (`after_stop`):** Created `/run/smartgateway.disabled`, stopped `gateway` & `supervisor` (`SUPERVISOR_COUNT_AFTER_STOP_5S=0`, `status` `rc=1`), remained stopped after 5s, manifest **15/15 MATCH**.
3. **`S99smartgateway start` (`after_start`):** Cleared `/run/smartgateway.disabled`, resumed `supervisor: running (pid=876 ppid=1)` & `gateway: running (pid=1300)`, manifest **15/15 MATCH**.
4. **Crash Self-Healing (`kill -9 876`, `after_crash_respawn`):** SysV `PID 1` automatically respawned `service_supervisor` (`pid=1536 ppid=1`), re-adopting `smart_gateway` (`pid=1300`), manifest **15/15 MATCH**.
5. **Untouched Factory `p2`:** `mount -o ro /dev/mmcblk1p2` confirmed stock `8,873,376` B `zImage` and `5,408` B `/sbin/init`.
6. **Normal Steady-State `reboot` (`post_reboot`):** Rebooted cleanly into `root=/dev/mmcblk1p1`, `sgw_ab_state=confirmed`, `bootcount=0`, `free -m`: `497` total / `477` available MiB (`32 MiB` CMA), `supervisor: pid=258 ppid=1`, `gateway: pid=272`, `eth0` `192.168.10.3/24`, `sgw_ap3216` (`als_raw=0`, `read_success=1`, `read_errors=0`), `/dev/sgw_event0` ready, manifest **15/15 MATCH**.

## 5. Parallel G4 Isolated Virtual-Disk `.swu` Verification (Host-Isolated Pass 5/5; Production SWUpdate Entry Pending)

Implemented `bsp/ota/g4_swu_tool.py` (`pack` / `apply`) and `tests/test_g4_swu_isolated.py` (5/5 OK inside `discover` suite), exercising on virtual `slot1.ext4`, `slot2.ext4`, `data.ext4` (`fuse2fs`), and redundant `emmc_env.img` (`0xE0000`/`0xE2000`) via real `fw_printenv`/`fw_setenv` and `bsp/apps/boot_ab/r3c_confirm_harness`:
- `p1 -> p2` (matching board `active=1, last_good=1`), `p2 -> p1`, and real clean G3 artifact `.swu` (`zImage` + `100ask_imx6ull-14x14.dtb` + `128 MiB rootfs.ext4`) -> RSA-2048 `sw-description.sig` verification -> board/payload checks -> target-slot & capacity guard -> `r3c_confirm_harness write-and-stage` (`debugfs` 15-file verify, write + `fsync` + byte-for-byte readback SHA-256, `PENDING` txn & redundant U-Boot Env `trial` stage) with source slot untouched.
- Rejected with **zero mutation** to slot images and `emmc_env.img`: untrusted RSA key, tampered `rootfs.img`, tampered `zImage`, wrong `board`, wrong target slot (`target=1` == `running=1` == `last_good=1`), slot capacity smaller than image, and `cpio` path traversal.

## 6. Archived Evidence

- `bsp/docs/validation/platform/g3-20261118-recheck-logs/g3_recheck_uart.log` (885 lines, MACs/EUI-64 redacted)
- `bsp/docs/validation/platform/g3-20261118-recheck-logs/g3_recheck_board.log`
- `bsp/docs/validation/platform/g3-20261118-recheck-logs/g3_recheck_txn.txt`
- `bsp/docs/validation/platform/g3-20261118-recheck-logs/g3_recheck_rootfs.manifest`
- `bsp/docs/validation/platform/g3-20261118-recheck-logs/g3_kernel_metrics.json`
- `bsp/docs/validation/platform/g3-20261118-recheck-logs/g3_rootfs_metrics.json`
- `bsp/docs/validation/platform/g3-20261118-recheck-logs/g3_g4_host_python.log`
