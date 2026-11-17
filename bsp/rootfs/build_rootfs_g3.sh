#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BR_SRC="${BUILDROOT_SRC_DIR:-$HOME/Desktop/Buildroot_2020.02.x}"
ARM_SDK_DIR="${ARM_SDK_DIR:-$HOME/Desktop/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot}"
OUT_DIR="${ROOTFS_G3_BUILD_DIR:-$REPO_DIR/build/buildroot-g3}"
DEFCONFIG_TMPL="$SCRIPT_DIR/g3_trimmed_defconfig"
POST_BUILD="$SCRIPT_DIR/post_build_g3.sh"
JOBS="${JOBS:-$(nproc)}"

if [ ! -d "$BR_SRC/package" ]; then
    echo "ERROR: Buildroot source not found at $BR_SRC" >&2
    exit 1
fi
if [ ! -x "$ARM_SDK_DIR/bin/arm-buildroot-linux-gnueabihf-gcc" ]; then
    echo "ERROR: External Buildroot SDK not found at $ARM_SDK_DIR" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
TMP_DEF="$OUT_DIR/g3_trimmed_resolved_defconfig"
sed "s|/REPLACE_WITH_ARM_SDK_DIR|$ARM_SDK_DIR|g" "$DEFCONFIG_TMPL" > "$TMP_DEF"
cat >> "$TMP_DEF" <<EOF
BR2_ROOTFS_POST_BUILD_SCRIPT="$POST_BUILD"
BR2_GLOBAL_PATCH_DIR="$SCRIPT_DIR/br_patches"
# Signed rootfs manifest verification invokes debugfs on the target.
BR2_PACKAGE_E2FSPROGS_DEBUGFS=y
EOF

export BR2_DL_DIR="$BR_SRC/dl"
export ac_cv_path_mkdir=/bin/mkdir
export MKDIR_P="/bin/mkdir -p"
export SGW_BSP_REPO="$REPO_DIR"
export SGW_VENDOR_OVERLAY="$BR_SRC/board/100ask/nxp-imx6ull/rootfs-overlay/systemV"

echo "=== Configuring Buildroot G3 Trimmed RootFS (O=$OUT_DIR) ==="
make -C "$BR_SRC" O="$OUT_DIR" BR2_DEFCONFIG="$TMP_DEF" defconfig >/dev/null
make -C "$BR_SRC" O="$OUT_DIR" olddefconfig >/dev/null

HOST_GCC_WRAPPER="$OUT_DIR/host-gcc-gnu11"
cat > "$HOST_GCC_WRAPPER" <<'EOF'
#!/bin/sh
exec /usr/bin/gcc -std=gnu11 -Wno-implicit-function-declaration -Wno-incompatible-pointer-types -Wno-int-conversion -Wno-return-mismatch -fcommon "$@"
EOF
chmod +x "$HOST_GCC_WRAPPER"

HOST_GXX_WRAPPER="$OUT_DIR/host-gxx-compat"
cat > "$HOST_GXX_WRAPPER" <<'EOF'
#!/bin/sh
exec /usr/bin/g++ -include cstdint "$@"
EOF
chmod +x "$HOST_GXX_WRAPPER"

echo "=== Building Buildroot G3 Trimmed RootFS (offline dl cache + external SDK) ==="
t0="$(date +%s)"
make -C "$OUT_DIR" -j"$JOBS" \
    HOSTCC="$HOST_GCC_WRAPPER" \
    HOSTCXX="$HOST_GXX_WRAPPER" \
    HOST_CFLAGS="-O2" \
    ac_cv_func_pidfd_open=no \
    ac_cv_func_pidfd_send_signal=no \
    ac_cv_path_mkdir=/bin/mkdir \
    MKDIR_P="/bin/mkdir -p"
t1="$(date +%s)"
echo "$((t1 - t0))" > "$OUT_DIR/build_seconds"

python3 - "$OUT_DIR" "$BR_SRC" <<'PYEOF'
import json
import subprocess
import sys
from pathlib import Path

out_dir = Path(sys.argv[1])
br_src = Path(sys.argv[2])
imgs = out_dir / "images"
tar_path = imgs / "rootfs.tar"
ext4_path = imgs / "rootfs.ext4"

# Measure actual payload bytes inside rootfs.tar (independent of ext4 container size 128M)
tar_list = subprocess.run(["tar", "-tvf", str(tar_path)], capture_output=True, text=True, check=True).stdout.splitlines()
payload_bytes = 0
for line in tar_list:
    parts = line.split()
    if len(parts) >= 3 and parts[0].startswith("-"):
        try:
            payload_bytes += int(parts[2])
        except ValueError:
            pass

# Check critical files exist inside rootfs.tar
names = {line.split()[-1].lstrip("./") for line in tar_list if line.split()}
required = [
    "boot/zImage",
    "boot/100ask_imx6ull-14x14.dtb",
    "usr/bin/adbd",
    "usr/bin/swupdate",
    "usr/bin/sgw_bootctl",
    "usr/bin/sgw_watchdog_guard",
    "usr/bin/service_supervisor",
    "sbin/fw_printenv",
    "etc/fw_env.config",
    "etc/init.d/S15sgw_ab_guard",
    "etc/init.d/S99adbd",
    "etc/init.d/S99smartgateway",
    "usr/bin/gdbserver",
]
missing = [r for r in required if r not in names]
if missing:
    raise RuntimeError(f"Missing required files in rootfs.tar: {missing}")

import tempfile
import shutil

cfg_lines = (out_dir / ".config").read_text(encoding="utf-8").splitlines()
trimmed_pkgs = sorted(l.split("=")[0] for l in cfg_lines if l.startswith("BR2_PACKAGE_") and l.endswith("=y"))

trimmed_dc_lines = (out_dir.parent.parent / "bsp/rootfs/g3_trimmed_defconfig").read_text(encoding="utf-8").splitlines()
trimmed_dc_pkgs = sorted(l.split("=")[0] for l in trimmed_dc_lines if l.startswith("BR2_PACKAGE_") and l.endswith("=y"))

orig_core_path = br_src / "configs/100ask_imx6ull_pro_ddr512m_systemV_core_defconfig"
orig_core = orig_core_path.read_text(encoding="utf-8").splitlines()
baseline_dc_pkgs = sorted(l.split("=")[0] for l in orig_core if l.startswith("BR2_PACKAGE_") and l.endswith("=y"))

with tempfile.TemporaryDirectory(prefix="br_core_cmp_") as td:
    subprocess.run(
        ["make", "-C", str(br_src), f"O={td}", f"BR2_DEFCONFIG={orig_core_path}", "defconfig"],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    resolved_core = (Path(td) / ".config").read_text(encoding="utf-8").splitlines()
baseline_resolved_pkgs = sorted(l.split("=")[0] for l in resolved_core if l.startswith("BR2_PACKAGE_") and l.endswith("=y"))

removed_resolved = sorted(set(baseline_resolved_pkgs) - set(trimmed_pkgs))
added_resolved = sorted(set(trimmed_pkgs) - set(baseline_resolved_pkgs))
diff_lines = ["# G3 Trimmed vs 100ask_imx6ull_pro_ddr512m_systemV_core_defconfig (resolved .config BR2_PACKAGE_*=y)"]
diff_lines += [f"- {p}" for p in removed_resolved]
diff_lines += [f"+ {p}" for p in added_resolved]
(out_dir.parent.parent / "bsp/rootfs/g3_trimmed_vs_core_packages.diff").write_text("\n".join(diff_lines) + "\n", encoding="utf-8")

metrics = {
    "ext4_image_bytes": ext4_path.stat().st_size,
    "tar_archive_bytes": tar_path.stat().st_size,
    "payload_files_bytes": payload_bytes,
    "tar_entry_count": len(tar_list),
    "trimmed_defconfig_package_count": len(trimmed_dc_pkgs),
    "baseline_core_defconfig_package_count": len(baseline_dc_pkgs),
    "trimmed_br2_package_count": len(trimmed_pkgs),
    "baseline_core_resolved_package_count": len(baseline_resolved_pkgs),
    "removed_packages_count": len(removed_resolved),
    "removed_packages_sample": removed_resolved[:40],
    "build_seconds": int((out_dir / "build_seconds").read_text(encoding="utf-8").strip()),
    "required_files_verified": required,
}
(out_dir / "g3_rootfs_metrics.json").write_text(json.dumps(metrics, indent=2), encoding="utf-8")
print(json.dumps(metrics, indent=2))
PYEOF
