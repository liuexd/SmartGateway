#!/usr/bin/env python3
"""G4 SWUpdate (.swu) packager and production SWUpdate entrypoint launcher."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SWUPDATE = ROOT / "build/swupdate-host/swupdate"
DEFAULT_BOOTCTL_ISOLATED = ROOT / "bsp/apps/boot_ab/sgw_bootctl_isolated"
DEFAULT_HOOK_SCRIPT = ROOT / "bsp/ota/sgw-swu-apply.sh"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            buf = f.read(1024 * 1024)
            if not buf:
                break
            h.update(buf)
    return h.hexdigest()


def cmd_pack(args):
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]*", args.board):
        raise RuntimeError(f"invalid board identifier for sw-description: {args.board!r}")
    if not re.fullmatch(r"[A-Za-z0-9._+-]+", args.version):
        raise RuntimeError(f"invalid version string for sw-description: {args.version!r}")
    if not re.fullmatch(r"[A-Za-z0-9._+-]+", args.hw_rev):
        raise RuntimeError(f"invalid hw_rev string for sw-description: {args.hw_rev!r}")

    rootfs_img = Path(args.rootfs_img).resolve()
    zimage = Path(args.zimage).resolve()
    dtb = Path(args.dtb).resolve()
    rootfs_manifest = Path(args.rootfs_manifest).resolve()
    privkey = Path(args.privkey).resolve()
    hook_script = Path(args.hook_script).resolve()
    out_swu = Path(args.out).resolve()

    for p in (rootfs_img, zimage, dtb, rootfs_manifest, privkey, hook_script):
        if not p.is_file():
            raise RuntimeError(f"missing input file: {p}")

    out_swu.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="g4-pack-") as td:
        stage = Path(td)
        shutil.copyfile(rootfs_img, stage / "rootfs.img")
        shutil.copyfile(zimage, stage / "zImage")
        shutil.copyfile(dtb, stage / "100ask_imx6ull-14x14.dtb")
        shutil.copyfile(rootfs_manifest, stage / "rootfs.manifest")
        shutil.copyfile(hook_script, stage / "sgw-swu-apply.sh")

        img_size = (stage / "rootfs.img").stat().st_size
        img_sha = sha256_file(stage / "rootfs.img")
        rm_sha = sha256_file(stage / "rootfs.manifest")
        z_sha = sha256_file(stage / "zImage")
        dtb_sha = sha256_file(stage / "100ask_imx6ull-14x14.dtb")
        hook_sha = sha256_file(stage / "sgw-swu-apply.sh")

        manifest_txt = (
            "SGW-SYSTEM-2\n"
            f"board={args.board}\n"
            "image=rootfs.img\n"
            f"size={img_size}\n"
            f"sha256={img_sha}\n"
            f"rootfs_manifest_sha256={rm_sha}\n"
        )
        (stage / "manifest").write_text(manifest_txt, encoding="utf-8")
        subprocess.run(
            ["openssl", "dgst", "-sha256", "-sign", str(privkey),
             "-out", str(stage / "manifest.sig"), str(stage / "manifest")],
            check=True, capture_output=True,
        )

        m_sha = sha256_file(stage / "manifest")
        msig_sha = sha256_file(stage / "manifest.sig")

        sw_desc_txt = (
            "software =\n"
            "{\n"
            f"\t{args.board} = {{\n"
            f'\t\tversion = "{args.version}";\n'
            '\t\tdescription = "SmartGateway G4 A/B system update";\n'
            f'\t\thardware-compatibility = [ "{args.hw_rev}" ];\n'
            "\t\tfiles: (\n"
            "\t\t\t{\n"
            '\t\t\t\tfilename = "manifest";\n'
            '\t\t\t\tpath = "bundle/manifest";\n'
            '\t\t\t\ttype = "rawfile";\n'
            f'\t\t\t\tsha256 = "{m_sha}";\n'
            "\t\t\t},\n"
            "\t\t\t{\n"
            '\t\t\t\tfilename = "manifest.sig";\n'
            '\t\t\t\tpath = "bundle/manifest.sig";\n'
            '\t\t\t\ttype = "rawfile";\n'
            f'\t\t\t\tsha256 = "{msig_sha}";\n'
            "\t\t\t},\n"
            "\t\t\t{\n"
            '\t\t\t\tfilename = "rootfs.manifest";\n'
            '\t\t\t\tpath = "bundle/rootfs.manifest";\n'
            '\t\t\t\ttype = "rawfile";\n'
            f'\t\t\t\tsha256 = "{rm_sha}";\n'
            "\t\t\t},\n"
            "\t\t\t{\n"
            '\t\t\t\tfilename = "zImage";\n'
            '\t\t\t\tpath = "bundle/zImage";\n'
            '\t\t\t\ttype = "rawfile";\n'
            f'\t\t\t\tsha256 = "{z_sha}";\n'
            "\t\t\t},\n"
            "\t\t\t{\n"
            '\t\t\t\tfilename = "100ask_imx6ull-14x14.dtb";\n'
            '\t\t\t\tpath = "bundle/100ask_imx6ull-14x14.dtb";\n'
            '\t\t\t\ttype = "rawfile";\n'
            f'\t\t\t\tsha256 = "{dtb_sha}";\n'
            "\t\t\t},\n"
            "\t\t\t{\n"
            '\t\t\t\tfilename = "rootfs.img";\n'
            '\t\t\t\tpath = "bundle/rootfs.img";\n'
            '\t\t\t\ttype = "rawfile";\n'
            f'\t\t\t\tsha256 = "{img_sha}";\n'
            "\t\t\t}\n"
            "\t\t);\n"
            "\t\tscripts: (\n"
            "\t\t\t{\n"
            '\t\t\t\tfilename = "sgw-swu-apply.sh";\n'
            '\t\t\t\ttype = "shellscript";\n'
            f'\t\t\t\tdata = "{int(args.target_slot)} {args.board} {args.version}";\n'
            f'\t\t\t\tsha256 = "{hook_sha}";\n'
            "\t\t\t}\n"
            "\t\t);\n"
            "\t};\n"
            "}\n"
        )
        (stage / "sw-description").write_text(sw_desc_txt, encoding="utf-8")
        subprocess.run(
            ["openssl", "dgst", "-sha256", "-sign", str(privkey),
             "-out", str(stage / "sw-description.sig"), str(stage / "sw-description")],
            check=True, capture_output=True,
        )

        # SWUpdate requires sw-description 1st and sw-description.sig 2nd in the CPIO stream.
        entries = [
            "sw-description",
            "sw-description.sig",
            "sgw-swu-apply.sh",
            "manifest",
            "manifest.sig",
            "rootfs.manifest",
            "zImage",
            "100ask_imx6ull-14x14.dtb",
            "rootfs.img",
        ]
        cpio_input = ("\n".join(entries) + "\n").encode("utf-8")
        with out_swu.open("wb") as out_f:
            subprocess.run(
                ["cpio", "-o", "-H", "crc", "--quiet"],
                cwd=str(stage), input=cpio_input, stdout=out_f, check=True,
            )
    print(f"PACKED_SWU {out_swu} target_slot={args.target_slot} img_sha256={img_sha}")


def cmd_apply(args):
    swu_path = Path(os.path.abspath(args.swu))
    pubkey = Path(os.path.abspath(args.pubkey))
    slot_img = Path(os.path.abspath(args.slot_img))
    txdir = Path(os.path.abspath(args.txdir))
    fw_cfg = Path(os.path.abspath(args.fw_cfg))
    fw_printenv = Path(os.path.abspath(args.fw_printenv))
    fw_setenv = Path(os.path.abspath(args.fw_setenv)) if args.fw_setenv else fw_printenv.with_name("fw_setenv")
    cmdline_path = Path(os.path.abspath(args.cmdline))
    mounts_path = Path(os.path.abspath(args.mounts))
    mountinfo_path = Path(os.path.abspath(args.mountinfo))
    sysroot_path = Path(os.path.abspath(args.sysroot))
    swupdate_bin = Path(os.path.abspath(args.swupdate_bin))
    bootctl_bin = Path(os.path.abspath(args.bootctl_bin))

    for p in (swu_path, pubkey, txdir, fw_cfg, fw_printenv,
              cmdline_path, mounts_path, mountinfo_path, sysroot_path,
              swupdate_bin, bootctl_bin):
        if not p.exists():
            raise RuntimeError(f"missing required path: {p}")

    workdir = Path(tempfile.mkdtemp(prefix=".g4-", dir=str(Path.home())))
    try:
        workdir.chmod(0o700)
        tdir = workdir / "t"
        bdir = workdir / "bundle"
        tdir.mkdir(mode=0o700)
        bdir.mkdir(mode=0o700)

        if args.app_ota_dir:
            app_ota_dir = Path(os.path.abspath(args.app_ota_dir))
        else:
            app_ota_dir = workdir / "app"
            app_ota_dir.mkdir(mode=0o700)
            (app_ota_dir / "state").mkdir(mode=0o700)
            (app_ota_dir / "releases/0.6.0").mkdir(parents=True, mode=0o700)
            lockf = app_ota_dir / ".ota.lock"
            lockf.write_bytes(b"")
            lockf.chmod(0o600)
            statef = app_ota_dir / "state/ota_state.json"
            statef.write_text(
                '{"schema_version":1,"state":"CONFIRMED","confirmed_version":"0.6.0","candidate_version":"0.6.0"}\n',
                encoding="utf-8",
            )
            statef.chmod(0o600)
            (app_ota_dir / "current").symlink_to("releases/0.6.0")

        data_mnt = Path(os.path.abspath(args.data_mnt)) if args.data_mnt else txdir.parent

        if args.policy:
            policy_path = Path(os.path.abspath(args.policy))
        else:
            policy_path = workdir / "r3c-policy.conf"
            policy_path.write_text(
                f"data_mountpoint={data_mnt}\n"
                "data_device=/dev/mmcblk1p3\n"
                "data_min_bytes=1048576\n"
                f"app_ota_dir={app_ota_dir}\n"
                f"tx_dir={txdir}\n"
                f"pubkey_path={pubkey}\n"
                "slot1_device=/dev/mmcblk1p1\n"
                "slot2_device=/dev/mmcblk1p2\n"
                "whole_disk_device=/dev/mmcblk1\n"
                f"supervisor_sock={app_ota_dir / 'run/supervisor.sock'}\n"
                f"supervisor_exe={app_ota_dir / 'bin/service_supervisor'}\n"
                f"gateway_sock={app_ota_dir / 'run/gateway-health.sock'}\n"
                f"gateway_exe={app_ota_dir / 'current/smart_gateway'}\n"
                "net_iface=eth0\n"
                "gateway_flow_mode=controlled_local\n"
                "allow_external_degraded=1\n"
                "require_sensor_heartbeat=0\n"
                f"protect_slot2={'0' if args.allow_virtual_slot2 else '1'}\n"
                "stable_ms=150\n"
                "max_gap_ms=400\n"
                "deadline_ms=2000\n"
                "max_evidence_age_ms=2000\n",
                encoding="utf-8",
            )
            policy_path.chmod(0o600)

        child_env = os.environ.copy()
        child_env["TMPDIR"] = str(tdir)
        child_env["SGW_FW_PRINTENV"] = str(fw_printenv)
        child_env["SGW_FW_SETENV"] = str(fw_setenv)
        child_env["SGW_SWU_ISOLATED_BOOTCTL"] = str(bootctl_bin)
        child_env["SGW_SWU_POLICY"] = str(policy_path)
        child_env["SGW_SWU_SLOT_IMG"] = str(slot_img)
        child_env["SGW_SWU_FW_CFG"] = str(fw_cfg)
        child_env["SGW_SWU_CMDLINE"] = str(cmdline_path)
        child_env["SGW_SWU_MOUNTS"] = str(mounts_path)
        child_env["SGW_SWU_MOUNTINFO"] = str(mountinfo_path)
        child_env["SGW_SWU_SYSROOT"] = str(sysroot_path)
        child_env["SGW_SWU_FUSE_DATA"] = "0" if args.no_fuse_data else "1"
        if args.expected_target is not None:
            child_env["SGW_SWU_EXPECTED_TARGET"] = str(int(args.expected_target))
        else:
            child_env.pop("SGW_SWU_EXPECTED_TARGET", None)

        cmd = [str(swupdate_bin), "-v"]
        if args.swupdate_cfg:
            cmd.extend(["-f", os.path.abspath(args.swupdate_cfg)])
        cmd.extend([
            "-i", str(swu_path),
            "-k", str(pubkey),
            "-H", f"{args.board}:{args.hw_rev}",
            "-M",
        ])

        res = subprocess.run(
            cmd,
            cwd=str(workdir),
            env=child_env,
            capture_output=True,
            text=True,
            timeout=240,
        )
        if res.returncode != 0:
            raise RuntimeError(
                f"swupdate failed (rc={res.returncode}):\n{res.stdout}\n{res.stderr}".strip()
            )
        if res.stdout:
            print(res.stdout.strip())
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


def main():
    p = argparse.ArgumentParser(description="SmartGateway G4 .swu pack and SWUpdate entrypoint tool")
    sub = p.add_subparsers(dest="cmd", required=True)

    pk = sub.add_parser("pack")
    pk.add_argument("--rootfs-img", required=True)
    pk.add_argument("--zimage", required=True)
    pk.add_argument("--dtb", required=True)
    pk.add_argument("--rootfs-manifest", required=True)
    pk.add_argument("--privkey", required=True)
    pk.add_argument("--target-slot", type=int, choices=(1, 2), required=True)
    pk.add_argument("--version", default="g4-0.1.0")
    pk.add_argument("--board", default="imx6ull-100ask")
    pk.add_argument("--hw-rev", default="revA")
    pk.add_argument("--hook-script", default=str(DEFAULT_HOOK_SCRIPT))
    pk.add_argument("--out", required=True)

    ap = sub.add_parser("apply")
    ap.add_argument("--swu", required=True)
    ap.add_argument("--pubkey", required=True)
    ap.add_argument("--slot-img", required=True)
    ap.add_argument("--txdir", required=True)
    ap.add_argument("--fw-cfg", required=True)
    ap.add_argument("--fw-printenv", required=True)
    ap.add_argument("--fw-setenv", default=None)
    ap.add_argument("--cmdline", required=True)
    ap.add_argument("--mounts", required=True)
    ap.add_argument("--mountinfo", required=True)
    ap.add_argument("--sysroot", required=True)
    ap.add_argument("--board", default="imx6ull-100ask")
    ap.add_argument("--hw-rev", default="revA")
    ap.add_argument("--expected-target", type=int, default=None)
    ap.add_argument("--app-ota-dir", default=None)
    ap.add_argument("--data-mnt", default=None)
    ap.add_argument("--policy", default=None)
    ap.add_argument("--swupdate-cfg", default=None)
    ap.add_argument("--swupdate-bin", default=str(DEFAULT_SWUPDATE))
    ap.add_argument("--bootctl-bin", default=str(DEFAULT_BOOTCTL_ISOLATED))
    ap.add_argument("--allow-virtual-slot2", action="store_true")
    ap.add_argument("--no-fuse-data", action="store_true")

    args = p.parse_args()
    try:
        if args.cmd == "pack":
            cmd_pack(args)
        else:
            cmd_apply(args)
    except Exception as exc:
        print(f"G4 ERROR: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
