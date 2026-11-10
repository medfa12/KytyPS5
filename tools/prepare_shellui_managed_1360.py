"""Stage the 13.60 managed ShellUI image (voldemort prepare_shellui_managed.py parity).

Exposes the original managed PE images embedded in the firmware's AOT SPRX
files under <workspace>/_ShellUIManaged, without modifying the firmware dump:

  _ShellUIManaged/native/eboot.bin       PSM launcher (NPXS40087 eboot.bin)
  _ShellUIManaged/native/sce_sys/param.json
  _ShellUIManaged/common/*.dll           extracted managed PEs (+ .sprx companions,
                                         incl. libmonosgen-2.0.sprx passthrough)
  _ShellUIManaged/Application/*          PSM Application assemblies (app.exe, ...)
  _ShellUIManaged/rnps/staged_titles.json  titles with a staged RNPS overlay
  _ShellUIManaged/rnps/apps/<title>        fw app dir + decrypted bundle overlay

The launcher uses an audited 13.60 startup patch for 1920x1080 internal PSM
rendering. The FW7.00 script packed *plaintext JS* bundles; 13.60 runs
*Hermes bytecode*, so a staged bundle is the container's plaintext payload
(the bare HBC), which the guest's RNPS loader accepts through its
"no RNPSHEDR magic -> offset 0, size = buffer" pass-through.

Per-title overlay model (voldemort tools/run_shellui.ps1 parity): the fw
`system_ex_b/rnps/apps/<title>` directory is mounted read-only and the dump
ships NPXS40141's bundles still encrypted (kernel-side /dev/rnps decrypt).
Staging copies the fw directory and overlays files from
--rnps-source/<title>/, then lists the title in staged_titles.json so the
launcher mounts it over the fw path (longest-prefix wins in
MountPoints::ResolvePath).
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import zipfile

WORKSPACE = Path(__file__).resolve().parent.parent
from local_paths import configured_path
STAGE = WORKSPACE / "_ShellUIManaged"
RNPS_SOURCE = WORKSPACE / "_TempData/rnps_staged"

# Independently audited startup sites; same version label does not imply the
# same executable. Output pins constrain the complete transform, not just anchors.
LAUNCHER_PROFILES = {
    "active": {
        "source": "cba9fd3855b34708595738bbb7a7b0a5f8a53a6d5f4f6b66f2aacf4eb34b1a09",
        "output": "7e04d0389edcb0881183e20e2ee49c836d422617aecbef552d00a4e35b8d1451",
        "dimensions": 0x126ab, "store": 0x126cd, "config": 0x127cd,
        "call": "488dbd28fdffffe8872dd100",
    },
    "alternate": {
        "source": "f3068cd9769711f9962367026f0bbba28ac2fac24e34fad1f40714979c89e060",
        "output": "b4ce830af247ae6611af81dece472710fa728a5c6855280d609168b62d556f4c",
        "dimensions": 0x122fb, "store": 0x1231d, "config": 0x1241d,
        "call": "488dbd28fdffffe82713d100",
    },
}


def detect_launcher_profile(fw: Path) -> str:
    """Identify a known launcher without staging or modifying the dump."""
    launcher = fw / "system_ex_b/app/NPXS40087/eboot.bin"
    if not launcher.is_file():
        raise ValueError("Missing ShellUI launcher: system_ex_b/app/NPXS40087/eboot.bin")
    digest = hashlib.sha256()
    with launcher.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    matches = [name for name, spec in LAUNCHER_PROFILES.items() if digest.hexdigest() == spec["source"]]
    if len(matches) != 1:
        raise ValueError("Unsupported ShellUI launcher: no known firmware profile matches its SHA256")
    return matches[0]


def link_or_copy(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        return
    try:
        os.link(source, destination)
    except OSError:
        shutil.copyfile(source, destination)


def embedded_pe(data: bytes) -> bytes:
    start = 0
    while (start := data.find(b"MZ", start)) >= 0:
        if start + 64 <= len(data):
            pe = start + struct.unpack_from("<I", data, start + 60)[0]
            if pe + 24 <= len(data) and data[pe:pe + 4] == b"PE\0\0":
                sections = struct.unpack_from("<H", data, pe + 6)[0]
                optional_size = struct.unpack_from("<H", data, pe + 20)[0]
                table = pe + 24 + optional_size
                if optional_size >= 64 and sections <= 96 and table + 40 * sections <= len(data):
                    ends = [struct.unpack_from("<I", data, pe + 24 + 60)[0]]
                    for section in range(sections):
                        size, offset = struct.unpack_from("<II", data, table + section * 40 + 16)
                        ends.append(size + offset)
                    end = start + max(ends)
                    if end <= len(data) and b"BSJB" in data[start:end]:
                        return data[start:end]
        start += 2
    raise ValueError("no embedded managed PE image")


def prepare(source: Path, destination: Path) -> int:
    destination.mkdir(parents=True, exist_ok=True)
    count = 0
    for module in sorted(source.glob("*.sprx")):
        if not module.name.endswith((".dll.sprx", ".exe.sprx")):
            continue
        # Keep the native AOT companion available alongside its unmodified PE metadata.
        link_or_copy(module, destination / module.name)
        image = embedded_pe(module.read_bytes())
        target = destination / module.name.removesuffix(".sprx")
        if not target.exists() or target.read_bytes() != image:
            target.write_bytes(image)
        count += 1
    return count


def transform_launcher(original: bytes, profile: str = "active") -> tuple[bytes, dict]:
    spec = LAUNCHER_PROFILES[profile]
    if hashlib.sha256(original).hexdigest() != spec["source"]:
        raise ValueError(f"Unsupported ShellUI launcher: {profile} source SHA256 mismatch")
    image = bytearray(original)
    # NPXS40087 13.60: startup stores width/height at config +0x50/+0x54,
    # then passes config to libScePsm lWlBrUu77Kg (PLT 0xd25560).
    # PSM +0x2325ec checks these fields for 4K and +0x233023 copies them
    # into rendering state. Seven other 4K immediates are unrelated defaults.
    phoff = struct.unpack_from("<Q", image, 32)[0]
    phsize, phcount = struct.unpack_from("<HH", image, 54)
    loads = []
    for index in range(phcount):
        kind = struct.unpack_from("<I", image, phoff + index * phsize)[0]
        offset, vaddr = struct.unpack_from("<QQ", image, phoff + index * phsize + 8)
        filesz = struct.unpack_from("<Q", image, phoff + index * phsize + 32)[0]
        if kind == 1:
            loads.append((offset, vaddr, filesz))

    def checked_offset(rva: int, expected: bytes) -> int:
        for offset, vaddr, filesz in loads:
            if vaddr <= rva and rva + len(expected) <= vaddr + filesz:
                at = offset + rva - vaddr
                if image[at:at + len(expected)] == expected:
                    return at
                break
        raise ValueError(f"Unsupported ShellUI launcher: 13.60 render setup mismatch at 0x{rva:x}")

    at = checked_offset(spec["dimensions"], b"\x48\xb8" + struct.pack("<II", 3840, 2160))
    checked_offset(spec["store"], bytes.fromhex("48 89 85 78 fd ff ff"))
    checked_offset(spec["config"], bytes.fromhex(spec["call"]))
    image[at + 2:at + 10] = struct.pack("<II", 1920, 1080)
    if hashlib.sha256(image).hexdigest() != spec["output"]:
        raise ValueError("ShellUI launcher transformed SHA256 mismatch")
    return bytes(image), {
        "schema": 1, "profile": profile, "source_sha256": spec["source"],
        "output_sha256": spec["output"], "transform": "psm-startup-1920x1080-v1",
        "patches": [{"file_offset": at + 2, "rva": spec["dimensions"] + 2,
                     "before": original[at + 2:at + 10].hex(),
                     "after": image[at + 2:at + 10].hex()}],
        "native_mailbox16_executed": False,
    }


def prepare_launcher(fw: Path, stage: Path, profile: str = "active") -> None:
    source = fw / "system_ex_b/app/NPXS40087/eboot.bin"
    image, receipt = transform_launcher(source.read_bytes(), profile)
    print(f"Launcher internal PSM rendering: 3840x2160 -> 1920x1080 ({profile})")
    target = stage / "native/eboot.bin"
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists() or target.read_bytes() != image:
        # Replace rather than mutate a potentially hardlinked staged image.
        temporary = target.with_suffix(".bin.tmp")
        temporary.write_bytes(image)
        os.replace(temporary, target)
    metadata = target.parent / "sce_sys/param.json"
    metadata.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(fw / "system_ex_b/app/NPXS40087/sce_sys/param.json", metadata)
    (target.parent / "launcher-transform.json").write_text(
        json.dumps(receipt, indent=2) + "\n", encoding="utf-8")


def prepare_preinst_fonts(fw: Path, stage: Path) -> None:
    if (fw / "preinst/common/font").is_dir():
        return
    source = fw / "segments/font.zip"
    with zipfile.ZipFile(source) as archive:
        members = archive.namelist()
        if not members or len(set(members)) != len(members) or any(
                Path(name).name != name or not name.lower().endswith(".ttf") for name in members):
            raise ValueError("Unexpected firmware font archive layout")
        payloads = {name: archive.read(name) for name in members}
    destination = stage / "preinst/common/font"
    destination.mkdir(parents=True, exist_ok=True)
    for name, data in payloads.items():
        target = destination / name
        if not target.exists() or target.read_bytes() != data:
            temporary = target.with_suffix(".ttf.tmp")
            temporary.write_bytes(data)
            os.replace(temporary, target)
    receipt = {"archive_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
               "files": {name: hashlib.sha256(data).hexdigest() for name, data in payloads.items()}}
    (stage / "preinst/staging-source.json").write_text(
        json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(f"Firmware fonts: {len(payloads)} extracted from matching segments/font.zip")


def validate_rnps_source(fw: Path, stage: Path, source: Path) -> list:
    """Check the complete overlay set before replacing any staged title."""
    if not source.is_dir():
        raise ValueError(f"RNPS source is not a directory: {source}")
    overlays = sorted(p for p in source.iterdir() if p.is_dir())
    if not overlays:
        raise ValueError(f"RNPS source has no per-title directories: {source}")
    for overlay in overlays:
        if not re.fullmatch(r"[A-Z]{4}[0-9]{5}", overlay.name):
            raise ValueError(f"RNPS source must contain title directories, not {overlay.name!r}: {source}")
        fw_dir = fw / "system_ex_b/rnps/apps" / overlay.name
        if not fw_dir.is_dir():
            raise ValueError(f"RNPS overlay has no firmware title: {fw_dir}")
        target = (stage / "rnps/apps" / overlay.name).resolve()
        for original in (source.resolve(), overlay.resolve(), fw_dir.resolve()):
            if target == original or target in original.parents or original in target.parents:
                raise ValueError(f"RNPS staging destination overlaps its source: {target}")
    return overlays


def stage_rnps_apps(fw: Path, stage: Path, source: Path) -> list:
    """Stage per-title RNPS overlays and return the titles to mount.

    Each `source/<title>/` holds decrypted bundle files (e.g. the plaintext
    Hermes payload of `host.ps.bundle.hbc`). The staged directory starts as a
    copy of the firmware title directory, including nested assets, and is then
    overlaid with the decrypted files. Copies keep guest writes away from the
    firmware dump.
    """
    titles = []
    for overlay in validate_rnps_source(fw, stage, source):
        title = overlay.name
        target = stage / "rnps/apps" / title
        # A previous staging run may have left files that the overlay no longer
        # provides (e.g. after switching a title's bundle set). The directory
        # holds only staged copies, so removing it never modifies the firmware.
        if target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True, exist_ok=True)
        fw_dir = fw / "system_ex_b/rnps/apps" / title
        # `_delete.txt` lists firmware files this overlay replaces by omission
        # (e.g. a sibling bundle variant that exists only as an encrypted
        # container we cannot decrypt). It must contain the exact file names.
        delete_list = overlay / "_delete.txt"
        deletes = set()
        if delete_list.is_file():
            deletes = {
                line.strip()
                for line in delete_list.read_text(encoding="utf-8").splitlines()
                if line.strip() and not line.lstrip().startswith("#")
            }
        for entry in sorted(fw_dir.rglob("*")):
            relative = entry.relative_to(fw_dir)
            if entry.is_file() and not (len(relative.parts) == 1 and entry.name in deletes):
                destination = target / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(entry, destination)
        overridden = []
        for entry in sorted(overlay.iterdir()):
            if entry.is_file() and entry.name != "_delete.txt":
                destination = target / entry.name
                shutil.copyfile(entry, destination)
                overridden.append(entry.name)
        titles.append(title)
        print(f"RNPS overlay {title}: {len(overridden)} file(s) staged "
              f"({', '.join(overridden)})")
    return titles


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fw-root", type=Path, help="Extracted 13.60 firmware root (or KYTY_FIRMWARE_ROOT)")
    parser.add_argument("--stage", type=Path, default=STAGE, help="Staging output directory")
    parser.add_argument("--launcher-profile", choices=LAUNCHER_PROFILES, default="active",
                        help="Exact audited source image; alternate requires an isolated stage")
    parser.add_argument("--detect-profile", action="store_true",
                        help="Identify a known launcher and exit without writing staging files")
    parser.add_argument("--rnps-source", type=Path,
                        help="Per-title RNPS replay overlay directory (active defaults to workspace cache; alternate has none)")
    args = parser.parse_args()
    fw: Path = configured_path(args.fw_root)
    stage: Path = args.stage
    if args.detect_profile:
        try:
            print(detect_launcher_profile(fw))
        except ValueError as exc:
            raise SystemExit(str(exc)) from exc
        return
    if not (fw / "system_ex_b/app/NPXS40087/eboot.bin").is_file():
        raise SystemExit(f"Not a 13.60 firmware root: {fw}")
    rnps_source = args.rnps_source
    if rnps_source is None and args.launcher_profile == "active" and RNPS_SOURCE.is_dir():
        rnps_source = RNPS_SOURCE
    if rnps_source is not None:
        validate_rnps_source(fw, stage, rnps_source)
    if args.launcher_profile == "alternate":
        if stage.resolve() == STAGE.resolve():
            raise SystemExit("Alternate firmware requires a separate --stage directory")
        # link_or_copy intentionally retains existing files. Refuse an unlabelled
        # populated stage rather than accidentally retaining another tree's AOTs.
        marker = stage / "stage-source.json"
        identity = {"profile": "alternate", "fw_root": str(fw.resolve()),
                    "launcher_sha256": LAUNCHER_PROFILES["alternate"]["source"]}
        transform_launcher((fw / "system_ex_b/app/NPXS40087/eboot.bin").read_bytes(), "alternate")
        if marker.exists():
            if json.loads(marker.read_text(encoding="utf-8")) != identity:
                raise SystemExit("Alternate stage source identity mismatch")
        elif stage.exists() and any(stage.iterdir()):
            raise SystemExit("Alternate stage must be empty or have a matching stage-source.json")
        stage.mkdir(parents=True, exist_ok=True)
        marker.write_text(json.dumps(identity, indent=2) + "\n", encoding="utf-8")
    elif (stage / "stage-source.json").exists():
        raise SystemExit("Source-labelled alternate stage cannot be used for the active profile")
    prepare_launcher(fw, stage, args.launcher_profile)
    prepare_preinst_fonts(fw, stage)
    common = prepare(fw / "system_ex_b/common_ex/lib", stage / "common")
    # mscorlib: prefer the dump's raw copy over the embedded-PE extraction
    # (the raw file is what Mono successfully loads; see docs/shellui-1360.md).
    raw_mscorlib = fw / "system_ex_b/common_ex/lib/mscorlib.dll"
    if raw_mscorlib.is_file():
        target = stage / "common/mscorlib.dll"
        if not target.exists() or target.read_bytes() != raw_mscorlib.read_bytes():
            shutil.copyfile(raw_mscorlib, target)
    # Native P/Invoke dependencies share the managed assembly search directory.
    for module in sorted((fw / "system_ex_b/common_ex/lib").glob("*.sprx")):
        link_or_copy(module, stage / "common" / module.name)
    # Mono's SystemMonitor P/Invoke searches beside the managed assemblies.
    composite = stage / "common/libSceComposite.sprx"
    if not composite.exists():
        link_or_copy(fw / "system_b/priv/lib/libSceComposite.sprx", composite)
    # Mono's MONO_PATH (/common_ex/lib/mono/2.1) and AOT-cache shape must
    # survive the /common_ex/lib overlay below: mirror the fw mono tree.
    for root, _, files in os.walk(fw / "system_ex_b/common_ex/lib/mono"):
        for name in files:
            source = Path(root) / name
            link_or_copy(source, stage / "common/mono" / source.relative_to(
                fw / "system_ex_b/common_ex/lib/mono"))
    app = prepare(fw / "system_ex_b/app/NPXS40087/psm/Application", stage / "Application")
    # The 13.60 dump also ships the raw assemblies next to the AOT images.
    # Prefer those authoritative originals over the extracted embedded PEs
    # (e.g. raw app.exe is the full managed assembly).
    for raw in sorted((fw / "system_ex_b/app/NPXS40087/psm/Application").glob("*.dll")):
        target = stage / "Application" / raw.name
        if not target.exists() or target.read_bytes() != raw.read_bytes():
            shutil.copyfile(raw, target)
    raw_exe = fw / "system_ex_b/app/NPXS40087/psm/Application/app.exe"
    if raw_exe.is_file():
        target = stage / "Application/app.exe"
        if not target.exists() or target.read_bytes() != raw_exe.read_bytes():
            shutil.copyfile(raw_exe, target)
    # Mono looks for each assembly's AOT image beside the managed DLL
    # (app-dir `X.dll.sprx`) and under mono/aot-cache. The fw app dir only
    # ships 7 AOT images; mscorlib's lives in common_ex/lib: stage it too
    # (else "AOT: image ...mscorlib.dll.sprx not found" -> later abort).
    # KYTY_NO_AOT=1 skips this (JIT-only experiment builds).
    if os.environ.get("KYTY_NO_AOT") != "1":
        aot = fw / "system_ex_b/common_ex/lib/mscorlib.dll.sprx"
        for dest in (stage / "Application/mscorlib.dll.sprx",
                     stage / "aot-cache/amd64/mscorlib.dll.sprx"):
            if not dest.exists():
                link_or_copy(aot, dest)
    (stage / "rnps").mkdir(parents=True, exist_ok=True)
    staged_titles = stage_rnps_apps(fw, stage, rnps_source) if rnps_source else []
    (stage / "rnps/staged_titles.json").write_text(json.dumps(staged_titles), encoding="utf-8")
    print(f"ShellUI managed images: {common} system, {app} application; {stage}")
    print(f"RNPS staged titles: {staged_titles or 'none'}")


if __name__ == "__main__":
    main()
