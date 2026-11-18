# G2-R3C Host-Side Security Closure Report (2026-11-18)

**Status:** `HOST-ACCEPTED` (host-side code, cross-compilation, and negative/positive regression gates closed; physical i.MX6ULL board execution of G2 U-Boot/Watchdog/A-B confirmation remains **board-pending**).

## 1. Closed Findings

1. **Real Gateway Ingress-to-Forwarding Local Pipeline (`gateway/`)**
   - Upgraded `SGW-LOCAL/1` handling in [health_probe.c](../../../gateway/health_probe.c) and [health_probe.h](../../../gateway/health_probe.h) from a direct `upstream_queue` marker push to a loopback TCP roundtrip (`ingress_try_roundtrip_local_diag()`) via `127.0.0.1:7000` (`wifi_listen_fd` -> `wifi_client_worker` -> `line_parser_feed` -> `gateway_app_on_wifi_line` -> `message_json_build_data` -> `upstream_queue` -> `server_link_worker` -> `gateway_health_consume_local_marker`).
   - Added `is_loopback` peer-address verification in [wifi_client.c](../../../gateway/wifi_client.c) (`getpeername` checking `INADDR_LOOPBACK`) and zero-initialized in [bluetooth_worker.c](../../../gateway/bluetooth_worker.c).
   - Guarded `_DIAG` frames in [gateway_app.c](../../../gateway/gateway_app.c) (`_DIAG` accepted only when `is_loopback && gateway_health_check_pending_diag(&data) && gateway_health_device_manager_ok(...)`, without calling `gateway_app_touch_device()` or forwarding upstream).
   - Moved health listener polling into dedicated `health_thread` in [gateway_loop.c](../../../gateway/gateway_loop.c) with clean join ordering before queue/device teardown.
   - Added negative regression test `test_unsolicited_or_mismatched_diag_is_rejected_without_upstream_pollution` in [test_g2r3b_gateway_probe.py](../../../tests/test_g2r3b_gateway_probe.py).

2. **Cross-Namespace Mount Verification in Target Slot Writer (`bsp/apps/boot_ab/r3c_txn.c`)**
   - Factored out `scan_mountinfo_file()` and added `scan_proc_mount_namespaces()` (`target_conflicts_with_mountinfo_or_slots_at()`).
   - When `mountinfo_path` is unset (production mode), `r3c_txn_write_and_verify_slot()` scans `/proc/self/mountinfo` and iterates all numeric `/proc/<pid>/ns/mnt` entries, deduplicating mount namespaces by `(st_dev, st_ino)`.
   - Tolerates concurrently exited processes (`ENOENT`/`ESRCH`), fails closed on unreadable foreign namespaces or `mountinfo` (`EACCES`/`EPERM`/malformed), and rejects target device/sector overlap mounted in foreign mount namespaces.
   - Added `test_foreign_mount_namespace_conflict_rejected_and_vanished_pid_tolerated` in [test_review4_topology.py](../../../tests/test_review4_topology.py).

3. **Production Confirm Gate Enabled Fail-Closed (`bsp/apps/boot_ab/r3c_coordinator.c`)**
   - Removed the temporary `!ops` stub rejection (`"trusted end-to-end business health unavailable"`) so production `sgw_bootctl confirm` (`ops == NULL`) executes the full fail-closed verification chain (root privilege check, policy load from `/etc/smartgateway/r3c-policy.conf`, locked app OTA & txdir inspection, DATA block/topology/persistence verification, OS running slot & `fw_env` verification, RSA-2048 signed manifest/rootfs verification, Supervisor/Gateway/hardware/network stability sampling, freshness recheck, and crash-safe `CONFIRMING -> fw_setenv -> readback -> CONFIRMED(consumed=1)` sequence).
   - Note: `r3c_policy_init_default()` retains `require_sensor_heartbeat=1` (fail-closed) when no authenticated NODE04 heartbeat source is configured (`ops == NULL`), requiring either an authenticated sensor heartbeat provider or explicit policy setting `require_sensor_heartbeat=0`.
   - Added `r3c_coordinator_confirm_guarded()` ([r3c_coordinator.h](../../../bsp/apps/boot_ab/r3c_coordinator.h)) accepting an optional watchdog keepalive callback during stability sampling.
   - Updated `confirm-null-gate` in [r3c_confirm_harness.c](../../../bsp/apps/boot_ab/tests/r3c_confirm_harness.c) and `test_production_confirm_fails_closed_even_with_root_identity` in [test_review4_topology.py](../../../tests/test_review4_topology.py) (testing both `r3c_confirm_harness confirm-null-gate` and `sgw_bootctl confirm` under `LD_PRELOAD` `geteuid()=0` and non-root).

4. **Production Watchdog Guard Integration (`bsp/apps/boot_ab/sgw_watchdog_guard.c`, `bsp/apps/boot_ab/Makefile`)**
   - Linked host and ARM `sgw_watchdog_guard` binaries with `$(R3C_SRCS)` and `$(R3C_LIBS)` (`-ljson-c -lcrypto`).
   - In production mode (no `-c`/`--config`/`--run-dir`/`--cmdline`/`--mounts`/`--sysroot` or `SGW_FW_*` overrides), `sgw_watchdog_guard` requires `geteuid() == 0`, hardcodes production `/usr/sbin/fw_*`, `/etc/fw_env.config`, `/proc/*`, `/opt/smartgateway/run`, loads `/etc/smartgateway/r3c-policy.conf`, clamps deadline against remaining `--budget-ms`, and invokes `r3c_coordinator_confirm_guarded()` with live watchdog keepalive kicks.
   - Preserved `ab_verify_system_health()` behavior when explicit test-override flags are supplied for existing G2 host simulation tests, and added `test_production_watchdog_guard_fails_closed_without_overrides` in [test_g2_uboot_ab.py](../../../tests/test_g2_uboot_ab.py).

## 2. Verification Summary (Observed After Final Edit)

- **Gateway C & ARM:**
  - `make clean gateway test arm-check`: 10/10 unit tests PASS (`test_crc16`, `test_frame`, `test_frame_parser`, `test_line_parser`, `test_message_json`, `test_message_queue`, `test_node_store`, `test_device_manager`, `test_command_manager`, `test_tcp_send_timeout`); ARM `build/arm-d25/smart_gateway` verified `ELF 32-bit LSB executable, ARM, EABI5`, `GLIBC_2.28`.
  - `make gateway BUILD_DIR=build/r3b-gateway CFLAGS="-Wall -Wextra -Werror -std=c11 -pthread -DSGW_GATEWAY_HEALTH_TESTING"`: clean build with `-Werror`.
- **Service Supervisor C:**
  - `make -C bsp/apps/service_supervisor clean test`: `service_ipc_test` (`58` checks, `0` failures), `service_supervisor_test` (`35` checks, `0` failures); `make -C bsp/apps/service_supervisor all` succeeded.
- **Boot A/B C & ARM:**
  - `make -C bsp/apps/boot_ab clean test arm-check`: `ab_state_test` (`PASS=100 FAIL=0`), `fw_env_backend_test` (`PASS=31 FAIL=0`), `ab_health_test` (`FAIL=0`), `system_health_test` (`checks=195 failures=0`), `health_devices_test` (`checks=21 failures=0`), `health_network_test` (`checks=5 failures=0`), `r3c_binding_test` (`checks=16 failures=0`), `r3c_data_exclusion_test` (`PASS`), `test_g2r3c_data` (`7` OK), `test_g2r3c_signed_bundle` (`5` OK); ARM binaries (`sgw_bootctl`, `sgw_watchdog_guard`, `sgw_system_health`) verified `ELF32`, `Machine: ARM`, `GLIBC_2.28`.
- **Python Unittest Suites (with `SGW_TEST_FUSE2FS=/usr/bin/fuse2fs`):**
  - `tests.test_g2_uboot_ab` (8), `tests.test_g2r3_health` (7), `tests.test_g2r3b_gateway_probe` (4), `tests.test_g2r3c_data` (7), `tests.test_g2r3c_signed_bundle` (5), `tests.test_g2r3c_fuse_image` (1), `tests.test_g2r3c_n09_n10` (16), `tests.test_review4_topology` (3): `Ran 51 tests in 37.458s OK` (0 skipped, 0 failed) — log: [g2_r3c_review4_python.log](g2-r3c-review4-closure-host-logs/g2_r3c_review4_python.log).
  - `tests.test_g1_*` (7 modules): `Ran 32 tests in 0.468s OK` — log: [g1_python.log](g2-r3c-review4-closure-host-logs/g1_python.log).
- **Standalone Host E2E Suites (after `sh tests/setup_ota_test_env.sh`):**
  - `test_tcp_connect_bounded.py` (`6/6 PASS`), `test_s5_supervisor_e2e.py` (`33/33 PASS`), `test_s3_preflight_running.py` (`75/75 PASS`), `test_s3r4_recovery.py` (`38/38 PASS`), `test_s4_fault_matrix.py` (`67/67 PASS`), `test_ota_health_e2e.py` (`PASS: OTA L2 business health`), `test_ota_northbound_fault.py` (`27/27 PASS`) — log: [standalone_e2e.log](g2-r3c-review4-closure-host-logs/standalone_e2e.log).

## 3. Scope Boundaries & Remaining Items

- **Deferred to Phase 3 (G3):** `tests.test_review4_g3_bundle` and `tests.test_g3_kernel_rootfs` were not included in G2-R3C host sign-off because trimmed G3 kernel/rootfs builds (`bsp/kernel/build_kernel_g3.sh`, `bsp/rootfs/build_rootfs_g3.sh`) are not yet generated.
- **Pending Board Execution:** Physical i.MX6ULL board execution of G2 (U-Boot `2017.03` A/B + WDOG1 reset fallback, `sgw_bootctl` / `sgw_watchdog_guard` trial confirmation against real AP3216C/GPIO/ext4 `/opt/smartgateway/data`) and board-only init scripts (`test_l12_respawn.py`, `test_s5r2_stop_respawn.py`) remain to be executed in the board-level G2 step.
