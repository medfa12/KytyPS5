"""Stage an explicit 13.60 OOBE experiment without editing the firmware/common stage."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

from prepare_shellui_managed_1360 import link_or_copy


LEGACY = "Sce.Vsh.ShellUI.Legacy.dll.sprx"
SOURCE_SHA256 = "070de0f8845090812b0490c1cd0ee8fde7bd67cf607fa63b3b0260f1ef6352ae"
# BootManager.ResetInitialSetupPhase, VA 0x24a8f (file offset 0x28a8f).
# GetInt(SYSTEM_initialize, 1); if != 1: SetInt(..., 1); SetBootEvent(0x15, 0).
RESET_OFFSET = 0x28a8f
WRITE_OFFSET = 0x28aad
PROFILES = {
    "active": {
        "source": SOURCE_SHA256,
        "output": "5805043b24d6e6c782b6b39b5d8e0f8b956a2b579a8c0082852416b9ddb1d68e",
        "reset_offset": RESET_OFFSET, "reset_size": 47,
        "reset_sha256": "1556e7b866f0c0dd2d84b517be194f78b97bc5c89cdc0eaed9e6338aff0ac7b5",
        "write_offset": WRITE_OFFSET,
    },
    # Independently resolved through alternate Legacy's AOT method-reference
    # blobs: IsCronos -> Registry.GetInt(uint,int) -> Registry.SetInt(uint,int).
    # The following ManufacturingMode/setup-phase branches remain intact.
    "alternate": {
        "source": "fb6cc305e69345aa08d2d5e31e8f2c75ee817a2ac357de19d04b3df94b1f6837",
        "output": "edf75dd3e49cbeb740715c4653c78b2c9efc97be0b6f050158eebbaa757751c3",
        "reset_offset": 0x2a4e3,
        "reset_size": 53,
        "reset_sha256": "2df3c440e91babefd7750f63919d1087e933dc5a45aefd6fc431dd8184f91736",
        "write_offset": 0x2a50a,
    },
}


def transform(original: bytes, profile: str = "active") -> tuple[bytes, dict]:
    spec = PROFILES[profile]
    if hashlib.sha256(original).hexdigest() != spec["source"]:
        raise ValueError("Unsupported Legacy image: first-boot hash mismatch")
    at, size = spec["reset_offset"], spec["reset_size"]
    if len(original[at:at + size]) != size or hashlib.sha256(original[at:at + size]).hexdigest() != spec["reset_sha256"]:
        raise ValueError("Unexpected ResetInitialSetupPhase instructions")
    image = bytearray(original)
    at = spec["write_offset"]
    image[at:at + 5] = b"\x90" * 5
    if hashlib.sha256(image).hexdigest() != spec["output"]:
        raise ValueError("First-boot transformed SHA256 mismatch")
    return bytes(image), {
        "schema": 1, "profile": profile, "source_sha256": spec["source"],
        "output_sha256": spec["output"], "transform": "skip-startup-initialize-write-v1",
        "patches": [{"file_offset": at, "before": original[at:at + 5].hex(),
                     "after": image[at:at + 5].hex()}],
    }


def stage(source: Path, destination: Path, profile: str = "active"):
    source = source.resolve()
    destination = destination.resolve()
    if destination == source or source in destination.parents:
        raise ValueError("The first-boot stage must be separate from the source stage")
    original = (source / LEGACY).read_bytes()
    image, receipt = transform(original, profile)
    # link_or_copy retains existing companions. A reused run name must not mix
    # another firmware's DLLs/AOTs with this independently pinned Legacy patch.
    # Check before any writes, including replacement of the Legacy image.
    if destination.exists():
        for existing in destination.rglob("*"):
            if not existing.is_file():
                continue
            relative = existing.relative_to(destination)
            if relative.as_posix() in (LEGACY, "firstboot-transform.json"):
                continue
            expected = source / relative
            if not expected.is_file() or (not existing.samefile(expected) and
                                         existing.read_bytes() != expected.read_bytes()):
                raise ValueError(f"First-boot stage contains a mismatched companion: {relative}")
    destination.mkdir(parents=True, exist_ok=True)
    for entry in source.iterdir():
        if entry.name == LEGACY:
            continue
        target = destination / entry.name
        if entry.is_dir():
            shutil.copytree(entry, target, dirs_exist_ok=True,
                            copy_function=lambda src, dst: link_or_copy(Path(src), Path(dst)))
        else:
            link_or_copy(entry, target)
    target = destination / LEGACY
    # Unlink before writing: a prior staging run may have used hard links.
    target.unlink(missing_ok=True)
    target.write_bytes(image)
    (destination / "firstboot-transform.json").write_text(
        json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print("First-boot experiment: skipped Legacy startup SetInt(SYSTEM_initialize, 1); "
          "guest registry writes remain enabled")
    print(f"First-boot common stage: {destination}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--profile", choices=PROFILES, default="active")
    args = parser.parse_args()
    stage(args.source, args.destination, args.profile)
