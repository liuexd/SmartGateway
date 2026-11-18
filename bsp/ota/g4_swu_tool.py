#!/usr/bin/env python3
"""G4 SWUpdate (.swu) packager and isolated/board A/B writer bridge."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_HARNESS = ROOT / "bsp/apps/boot_ab/r3c_confirm_harness"
REQUIRED_SWU_ENTRIES = (
    "sw-description",
    "sw-description.sig",
    "manifest",
    "manifest.sig",
    "rootfs.manifest",
    "rootfs.img",
)
MANDATORY_ROOTFS_PATHS = (
    "/sbin/init", "/etc/inittab", "/etc/fstab", "/etc/fw_env.config", "/sbin/fw_printenv",
    "/usr/bin/sgw_bootctl", "/usr/bin/sgw_watchdog_guard", "/usr/bin/service_supervisor",
    "/usr/bin/smart_gateway", "/usr/bin/ota_agent", "/etc/init.d/S15sgw_ab_guard",
    "/etc/init.d/S99smartgateway", "/boot/zImage", "/boot/100ask_imx6ull-14x14.dtb",
    "/lib/libc.so.6",
)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            buf = f.read(1024 * 1024)
            if not buf:
                break
            h.update(buf)
    return h.hexdigest()


def parse_fw_env(fw_printenv: Path, fw_cfg: Path) -> dict:
    r = subprocess.run([str(fw_printenv), "-c", str(fw_cfg)], capture_output=True, text=True, check=True)
    out = {}
    for line in r.stdout.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def detect_running_slot_from_cmdline(cmdline_path: Path) -> int:
    txt = cmdline_path.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"root=/dev/mmcblk1p([12])\b", txt)
    if not m:
        raise RuntimeError(f"cannot determine running slot from cmdline {cmdline_path}: {txt.strip()!r}")
    return int(m.group(1))


def cmd_pack(args):
    rootfs_img = Path(args.rootfs_img).resolve()
    zimage = Path(args.zimage).resolve()
    dtb = Path(args.dtb).resolve()
    rootfs_manifest = Path(args.rootfs_manifest).resolve()
    privkey = Path(args.privkey).resolve()
    out_swu = Path(args.out).resolve()

    for p in (rootfs_img, zimage, dtb, rootfs_manifest, privkey):
        if not p.is_file():
            raise RuntimeError(f"missing input file: {p}")

    out_swu.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="g4-pack-") as td:
        stage = Path(td)
        shutil.copyfile(rootfs_img, stage / "rootfs.img")
        shutil.copyfile(zimage, stage / "zImage")
        shutil.copyfile(dtb, stage / "100ask_imx6ull-14x14.dtb")
        shutil.copyfile(rootfs_manifest, stage / "rootfs.manifest")

        img_size = (stage / "rootfs.img").stat().st_size
        img_sha = sha256_file(stage / "rootfs.img")
        rm_sha = sha256_file(stage / "rootfs.manifest")
        z_sha = sha256_file(stage / "zImage")
        z_size = (stage / "zImage").stat().st_size
        dtb_sha = sha256_file(stage / "100ask_imx6ull-14x14.dtb")
        dtb_size = (stage / "100ask_imx6ull-14x14.dtb").stat().st_size

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

        # Libconfig-compatible SWUpdate sw-description with structured artifact hashes/sizes
        sw_desc = {
            "schema": "SGW-SWU-1",
            "board": args.board,
            "version": args.version,
            "target_slot": int(args.target_slot),
            "images": [
                {"filename": "rootfs.img", "sha256": img_sha, "size": img_size},
                {"filename": "zImage", "sha256": z_sha, "size": z_size},
                {"filename": "100ask_imx6ull-14x14.dtb", "sha256": dtb_sha, "size": dtb_size},
                {"filename": "rootfs.manifest", "sha256": rm_sha, "size": (stage / "rootfs.manifest").stat().st_size},
            ],
        }
        sw_desc_txt = (
            f"/* SmartGateway G4 sw-description */\n"
            f"software =\n{{\n"
            f"    version = \"{args.version}\";\n"
            f"    board = \"{args.board}\";\n"
            f"    target_slot = {int(args.target_slot)};\n"
            f"    meta = {json.dumps(sw_desc, sort_keys=True)};\n"
            f"}}\n"
        )
        (stage / "sw-description").write_text(sw_desc_txt, encoding="utf-8")
        subprocess.run(
            ["openssl", "dgst", "-sha256", "-sign", str(privkey),
             "-out", str(stage / "sw-description.sig"), str(stage / "sw-description")],
            check=True, capture_output=True,
        )

        entries = [
            "sw-description",
            "sw-description.sig",
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
                ["cpio", "-o", "-H", "newc", "--quiet"],
                cwd=str(stage), input=cpio_input, stdout=out_f, check=True,
            )
    print(f"PACKED_SWU {out_swu} target_slot={args.target_slot} img_sha256={img_sha}")


def parse_sw_description(sw_desc_path: Path) -> dict:
    txt = sw_desc_path.read_text(encoding="utf-8")
    m = re.search(r"meta\s*=\s*(\{.*?\});", txt, flags=re.DOTALL)
    if not m:
        raise RuntimeError("malformed sw-description: missing meta stanza")
    meta = json.loads(m.group(1))
    if meta.get("schema") != "SGW-SWU-1":
        raise RuntimeError(f"unsupported sw-description schema: {meta.get('schema')!r}")
    return meta


def cmd_apply(args):
    swu_path = Path(args.swu).resolve()
    pubkey = Path(args.pubkey).resolve()
    slot_img = Path(args.slot_img).resolve()
    txdir = Path(args.txdir).resolve()
    fw_cfg = Path(args.fw_cfg).resolve()
    fw_printenv = Path(os.path.abspath(args.fw_printenv))
    cmdline_path = Path(args.cmdline).resolve()
    mounts_path = Path(args.mounts).resolve()
    mountinfo_path = Path(args.mountinfo).resolve()
    sysroot_path = Path(args.sysroot).resolve()
    harness = Path(args.harness).resolve()

    if not swu_path.is_file() or not pubkey.is_file():
        raise RuntimeError("missing swu or pubkey")

    # 1. Inspect cpio archive TOC safely before extraction
    toc = subprocess.run(
        ["cpio", "-it", "--quiet"],
        input=swu_path.read_bytes(), capture_output=True, check=True,
    )
    entries = [line.strip() for line in toc.stdout.decode("utf-8", errors="replace").splitlines() if line.strip()]
    if len(entries) != len(set(entries)):
        raise RuntimeError("duplicate entries detected inside .swu archive")
    for e in entries:
        if e.startswith("/") or ".." in Path(e).parts or "/" in e:
            raise RuntimeError(f"unsafe archive member path in .swu: {e!r}")
    missing = [req for req in REQUIRED_SWU_ENTRIES if req not in entries]
    if missing:
        raise RuntimeError(f".swu archive missing required entries: {missing}")

    with tempfile.TemporaryDirectory(prefix=".g4-apply-", dir=Path.home()) as td:
        base = Path(td)
        base.chmod(0o700)
        stage = base / "stage"
        stage.mkdir(mode=0o700)
        subprocess.run(
            ["cpio", "-i", "--no-absolute-filenames", "--quiet"],
            cwd=str(stage), input=swu_path.read_bytes(), check=True,
        )
        for item in stage.iterdir():
            st = item.lstat()
            if not stat.S_ISREG(st.st_mode):
                raise RuntimeError(f"non-regular extracted file in .swu: {item.name}")
            item.chmod(0o600)

        # 2. Verify RSA-2048 signature of sw-description BEFORE parsing it
        ver = subprocess.run(
            ["openssl", "dgst", "-sha256", "-verify", str(pubkey),
             "-signature", str(stage / "sw-description.sig"), str(stage / "sw-description")],
            capture_output=True, text=True,
        )
        if ver.returncode != 0:
            raise RuntimeError(f"sw-description signature verification failed: {ver.stdout} {ver.stderr}".strip())

        meta = parse_sw_description(stage / "sw-description")
        if meta.get("board") != args.board:
            raise RuntimeError(f"board mismatch: {meta.get('board')!r} != {args.board!r}")

        target_slot = int(meta["target_slot"])
        if target_slot not in (1, 2):
            raise RuntimeError(f"invalid target_slot in sw-description: {target_slot}")

        imgs = meta.get("images") or []
        by_name = {item["filename"]: item for item in imgs}
        for req_img in ("rootfs.img", "zImage", "100ask_imx6ull-14x14.dtb", "rootfs.manifest"):
            if req_img not in by_name:
                raise RuntimeError(f"sw-description missing image descriptor for {req_img}")
            fp = stage / req_img
            if not fp.is_file():
                raise RuntimeError(f".swu missing file listed in sw-description: {req_img}")
            actual_sz = fp.stat().st_size
            actual_sha = sha256_file(fp)
            if actual_sz != int(by_name[req_img]["size"]) or actual_sha != by_name[req_img]["sha256"]:
                raise RuntimeError(f"payload digest/size mismatch for {req_img}")

        # Verify 15 mandatory rootfs manifest entries exist in rootfs.manifest
        rm_lines = (stage / "rootfs.manifest").read_text(encoding="utf-8").splitlines()
        rm_paths = [line.split(" ", 1)[0] for line in rm_lines if line.strip()]
        for req_p in MANDATORY_ROOTFS_PATHS:
            if req_p not in rm_paths:
                raise RuntimeError(f"rootfs.manifest missing mandatory path {req_p}")

        # 3. Target-slot & capacity pre-write guards
        running_slot = detect_running_slot_from_cmdline(cmdline_path)
        env = parse_fw_env(fw_printenv, fw_cfg)
        last_good_slot = int(env.get("sgw_last_good_slot", "0"))
        if last_good_slot not in (1, 2):
            raise RuntimeError(f"invalid sgw_last_good_slot in fw_env: {last_good_slot}")
        if args.expected_target is not None and target_slot != int(args.expected_target):
            raise RuntimeError(f"target_slot {target_slot} != expected_target {args.expected_target}")
        if target_slot == running_slot:
            raise RuntimeError(f"REFUSED: target_slot {target_slot} equals currently running slot {running_slot}")
        if target_slot == last_good_slot:
            raise RuntimeError(f"REFUSED: target_slot {target_slot} equals last_good slot {last_good_slot}")

        if not slot_img.exists():
            raise RuntimeError(f"target slot device/image does not exist: {slot_img}")
        slot_cap = slot_img.stat().st_size
        payload_sz = (stage / "rootfs.img").stat().st_size
        if slot_cap < payload_sz:
            raise RuntimeError(f"target slot capacity {slot_cap} < rootfs.img size {payload_sz}")

        # 4. Delegate verified bundle dir to C r3c_confirm_harness (debugfs + write + fsync + readback + pending)
        bundle = base / "bundle"
        bundle.mkdir(mode=0o700)
        for fname in ("manifest", "manifest.sig", "rootfs.manifest", "rootfs.img"):
            shutil.copyfile(stage / fname, bundle / fname)
            (bundle / fname).chmod(0o600)

        fw_setenv = Path(os.path.abspath(args.fw_setenv)) if args.fw_setenv else fw_printenv.with_name("fw_setenv")
        child_env = os.environ.copy()
        child_env["SGW_FW_PRINTENV"] = str(fw_printenv)
        child_env["SGW_FW_SETENV"] = str(fw_setenv)
        res = subprocess.run(
            [
                str(harness), "write-and-stage",
                "--bundle", str(bundle),
                "--txdir", str(txdir),
                "--pubkey", str(pubkey),
                "--slot-img", str(slot_img),
                "--fw-cfg", str(fw_cfg),
                "--cmdline", str(cmdline_path),
                "--mounts", str(mounts_path),
                "--mountinfo", str(mountinfo_path),
                "--sysroot", str(sysroot_path),
                "--target", str(target_slot),
                "--last-good", str(last_good_slot),
            ],
            env=child_env, capture_output=True, text=True, timeout=180,
        )
        if res.returncode != 0:
            raise RuntimeError(f"r3c write-and-stage rejected: {res.stdout}\n{res.stderr}".strip())
        print(res.stdout.strip())


def main():
    p = argparse.ArgumentParser(description="SmartGateway G4 .swu pack and apply tool")
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
    ap.add_argument("--expected-target", type=int, default=None)
    ap.add_argument("--harness", default=str(DEFAULT_HARNESS))

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
