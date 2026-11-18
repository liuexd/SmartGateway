# G3 Clean Rebuild, Boot-Sequence Hardening & Host Acceptance (2026-11-18)

**Status:** `HOST-ACCEPTED` (clean-directory kernel + Buildroot rootfs rebuild, DTS GPIO/AP3216C integration, board-verified SysV `rcS`/`rcK`/BusyBox `inittab` hardening, `debugfs` size/attack-surface audit, and 54+32 Python regression pass; board candidate-slot flashing & boot verification of the G3 trimmed image is the immediate next step).

## 1. Hardened Changes Since Baseline G3

1. **Dual-Sensor Device Tree Integration ([0002-add-sgw-gpio-event-device-tree-node.patch](../../dts/patches/0002-add-sgw-gpio-event-device-tree-node.patch), [build_kernel_g3.sh](../../kernel/build_kernel_g3.sh)):**
   - Added `pinctrl_sgw_button` (`sgwbuttongrp`, `GPIO4_IO19`) and `sgw_gpio_event` (`compatible = "smartgateway,sgw-gpio-event"`) alongside `ap3216@1e` (`0001-add-ap3216-device-tree-node.patch`) during both baseline and trimmed G3 kernel builds, verified via `fdtget` (`ap3216_in_dtb=true`, `gpio_in_dtb=true`).
2. **Board-Verified RootFS Init & Runtime Layout ([post_build_g3.sh](../../rootfs/post_build_g3.sh)):**
   - **BusyBox `SYSINIT -> RESPAWN` sequencing:** Updated `/etc/init.d/rcS` to skip `*S99smartgateway` during the `S??*` loop and launch `(sleep 2; /etc/init.d/S99smartgateway start) </dev/null >/dev/console 2>&1 &` after `/bin/hostname`, preventing PID 1 deadlock while `rcS` finishes.
   - **Signed `/etc/inittab` digest stability:** Pre-populated the `::respawn:` `service_supervisor` entry once in `/etc/inittab` and added `/etc/init.d/rcK` stopping `service_supervisor` cleanly without stripping `::respawn:` during shutdown/reboot.
   - **Drivers, DATA mount, & `/opt/smartgateway`:** Installed `/etc/init.d/S12sgw_drivers` (`sgw_ap3216.ko`, `sgw_gpio_event.ko`), `/dev/mmcblk1p3 /opt/smartgateway/data ext4 defaults 0 2` in `/etc/fstab`, `/etc/smartgateway/system-health-network.conf` (`interface=eth0`), and `0700`/`0600` `/opt/smartgateway/` layout matching `ota_state.json`.
3. **Expanded G3 Regression Tests ([test_g3_kernel_rootfs.py](../../../tests/test_g3_kernel_rootfs.py)):**
   - Asserts `gpio_in_dtb`, `S12sgw_drivers`, `rcK`, `rcS` async `S99smartgateway`, `inittab` `::respawn:` line, `/opt/smartgateway/data` in `fstab`, and `/opt/smartgateway/{bin/service_supervisor,releases/0.6.0/smart_gateway}`.

## 2. Clean Rebuild Metrics & `debugfs` Audit

- **Kernel (`build/kernel-g3/g3_kernel_metrics.json`):**
  - `zImage`: `8,873,360 B` (baseline) -> `4,848,520 B` (trimmed, `-45.4%`)
  - `vmlinux .text`: `12,835,954 B` -> `6,769,702 B` (`-47.3%`)
  - Kernel modules: `78` modules / `3,304,232 B` -> `48` modules / `1,131,700 B` (`-65.8%`), plus 5 external BSP modules built (`bsp_hello.ko`, `devm_demo.ko`, `dt_match_demo.ko`, `sgw_ap3216.ko`, `sgw_gpio_event.ko`)
  - CMA: `320 MiB` -> `32 MiB`; `dtb_bytes`: `38,707 B` (`ap3216_in_dtb=true`, `gpio_in_dtb=true`).
- **RootFS (`build/buildroot-g3/g3_rootfs_metrics.json`):**
  - `rootfs.tar` payload: `32,594,940 B` (`~31.1 MiB`, `1,299` tar entries); `rootfs.ext4`: `134,217,728 B` (`128 MiB`, `-O ^metadata_csum` U-Boot 2017.03 compatible).
  - Packages: `35` vs `136` defconfig (`153` vs `315` resolved `BR2_PACKAGE_*=y`).
- **`debugfs` Footprint & Attack Surface Audit ([g3_key_entries.txt](g3-20261118-logs/g3_key_entries.txt)):**
  - `/sbin/debugfs` binary is `150,176 B` (~147 KiB, `~0.46%` of `rootfs.tar` payload); shared libraries `libext2fs.so.2.4` (`327,708 B`) and `libe2p.so.2.3` (`27,060 B`) are already required by `e2fsprogs` (`e2fsck`/`mke2fs`/`tune2fs`).
  - Security boundary: `r3c_bundle.c` invokes `debugfs` only read-only after RSA-2048 signature verification succeeds, via `execv()` without shell interpolation, under bounded `10s`/command and `60s` total deadlines (tested in `test_signature_rejected_before_debugfs_is_executed` and `test_debugfs_subprocess_timeout_rejects_bundle`).

## 3. Verification Results (After Clean Rebuild)

- **G3 + Review4 + G2 + R3B/R3C (`SGW_TEST_FUSE2FS=/usr/bin/fuse2fs`):** `54/54 OK` in `38.222s` (0 skipped, 0 failed) — [g3_full_python.log](g3-20261118-logs/g3_full_python.log).
  - Includes `test_g3_kernel_rootfs` (2), `test_review4_g3_bundle` (1, signed manifest verified against rebuilt `build/buildroot-g3/images/rootfs.ext4`), `test_review4_topology` (3), `test_g2_uboot_ab` (8), `test_g2r3_health` (7), `test_g2r3b_gateway_probe` (4), `test_g2r3c_data` (7), `test_g2r3c_signed_bundle` (5), `test_g2r3c_fuse_image` (1), `test_g2r3c_n09_n10` (16).
- **G1 Python Suites (7 modules):** `32/32 OK` in `0.45s+` — [g1_python.log](g3-20261118-logs/g1_python.log).
