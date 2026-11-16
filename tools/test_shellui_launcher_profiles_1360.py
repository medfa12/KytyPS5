"""Retained-input checks for the independently audited launcher transforms."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from prepare_shellui_managed_1360 import LAUNCHER_PROFILES, detect_launcher_profile, prepare_launcher, transform_launcher
from prepare_shellui_first_boot_1360 import LEGACY, PROFILES, stage as stage_first_boot, transform as transform_first_boot


class LauncherDetectionTests(unittest.TestCase):
    def test_detection_is_read_only_and_rejects_unknown(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            launcher = root / "system_ex_b/app/NPXS40087/eboot.bin"
            with self.assertRaisesRegex(ValueError, "Missing ShellUI launcher"):
                detect_launcher_profile(root)
            launcher.parent.mkdir(parents=True)
            launcher.write_bytes(b"synthetic launcher")
            with self.assertRaisesRegex(ValueError, "no known firmware profile"):
                detect_launcher_profile(root)
            digest = hashlib.sha256(launcher.read_bytes()).hexdigest()
            with patch.dict(LAUNCHER_PROFILES, {"synthetic": {"source": digest}}, clear=True):
                self.assertEqual(detect_launcher_profile(root), "synthetic")
            self.assertEqual(launcher.read_bytes(), b"synthetic launcher")
            self.assertEqual([p.name for p in root.iterdir()], ["system_ex_b"])


class LauncherProfileTests(unittest.TestCase):
    def setUp(self):
        self.roots = {
            "active": os.environ.get("SHELL_ACTIVE_FIRMWARE"),
            "alternate": os.environ.get("SHELL_ALT_FIRMWARE"),
        }
        if not all(self.roots.values()):
            self.skipTest("Set SHELL_ACTIVE_FIRMWARE and SHELL_ALT_FIRMWARE")
        self.roots = {key: Path(value) for key, value in self.roots.items()}

    def image(self, name):
        return (self.roots[name] / "system_ex_b/app/NPXS40087/eboot.bin").read_bytes()

    def test_detect_known_firmware_profiles(self):
        for name, root in self.roots.items():
            with self.subTest(profile=name):
                self.assertEqual(detect_launcher_profile(root), name)

    def test_exact_transform_and_roundtrip(self):
        for name, spec in LAUNCHER_PROFILES.items():
            with self.subTest(profile=name):
                source = self.image(name)
                result, receipt = transform_launcher(source, name)
                self.assertEqual(hashlib.sha256(result).hexdigest(), spec["output"])
                patch, = receipt["patches"]
                at = patch["file_offset"]
                self.assertEqual(result[:at], source[:at])
                self.assertEqual(result[at + 8:], source[at + 8:])
                restored = bytearray(result)
                restored[at:at + 8] = bytes.fromhex(patch["before"])
                self.assertEqual(restored, source)

    def test_cross_profile_and_modified_source_rejected(self):
        for name in self.roots:
            source = self.image(name)
            with self.assertRaisesRegex(ValueError, "source SHA256 mismatch"):
                transform_launcher(source, "alternate" if name == "active" else "active")
            modified = bytearray(source)
            modified[-1] ^= 1
            with self.assertRaisesRegex(ValueError, "source SHA256 mismatch"):
                transform_launcher(modified, name)
            patched, _ = transform_launcher(source, name)
            with self.assertRaisesRegex(ValueError, "source SHA256 mismatch"):
                transform_launcher(patched, name)

    def test_replacement_preserves_hardlinked_original(self):
        with tempfile.TemporaryDirectory() as temp:
            stage = Path(temp) / "stage"
            target = stage / "native/eboot.bin"
            target.parent.mkdir(parents=True)
            original = Path(temp) / "original.bin"
            source = self.image("alternate")
            original.write_bytes(source)
            os.link(original, target)
            prepare_launcher(self.roots["alternate"], stage, "alternate")
            self.assertEqual(original.read_bytes(), source)
            self.assertEqual(hashlib.sha256(target.read_bytes()).hexdigest(),
                             LAUNCHER_PROFILES["alternate"]["output"])

    def test_populated_unlabelled_stage_rejected_before_write(self):
        with tempfile.TemporaryDirectory() as temp:
            stage = Path(temp)
            sentinel = stage / "existing-aot.sprx"
            sentinel.write_bytes(b"existing stage")
            result = subprocess.run(
                [sys.executable, "-B", str(Path(__file__).with_name("prepare_shellui_managed_1360.py")),
                 "--fw-root", str(self.roots["alternate"]), "--stage", str(stage),
                 "--launcher-profile", "alternate"], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("stage must be empty", result.stderr)
            self.assertEqual(list(stage.iterdir()), [sentinel])
            self.assertEqual(sentinel.read_bytes(), b"existing stage")

    def test_first_boot_exact_patch_and_rejects_cross_profile(self):
        for name, root in self.roots.items():
            source = (root / "system_ex_b/common_ex/lib" / LEGACY).read_bytes()
            output, receipt = transform_first_boot(source, name)
            self.assertEqual(hashlib.sha256(output).hexdigest(), PROFILES[name]["output"])
            patch, = receipt["patches"]
            at = patch["file_offset"]
            self.assertEqual(output[:at], source[:at])
            self.assertEqual(output[at + 5:], source[at + 5:])
            self.assertEqual(output[at:at + 5], b"\x90" * 5)
            for bad, profile in [(source, "alternate" if name == "active" else "active"),
                                 (output, name), (source[:-1] + bytes([source[-1] ^ 1]), name)]:
                with self.assertRaisesRegex(ValueError, "hash mismatch"):
                    transform_first_boot(bad, profile)

    def test_first_boot_preserves_source_and_hardlink(self):
        original = (self.roots["alternate"] / "system_ex_b/common_ex/lib" / LEGACY).read_bytes()
        with tempfile.TemporaryDirectory() as temp:
            source, dest = Path(temp) / "common", Path(temp) / "firstboot"
            source.mkdir()
            dest.mkdir()
            (source / LEGACY).write_bytes(original)
            (source / "companion.dll").write_bytes(b"preserved companion")
            os.link(source / LEGACY, dest / LEGACY)
            stage_first_boot(source, dest, "alternate")
            self.assertEqual((source / LEGACY).read_bytes(), original)
            self.assertEqual((dest / "companion.dll").read_bytes(), b"preserved companion")
            self.assertEqual(hashlib.sha256((dest / LEGACY).read_bytes()).hexdigest(),
                             PROFILES["alternate"]["output"])

    def test_first_boot_rejects_mixed_companions_before_write(self):
        original = (self.roots["alternate"] / "system_ex_b/common_ex/lib" / LEGACY).read_bytes()
        with tempfile.TemporaryDirectory() as temp:
            source, dest = Path(temp) / "common", Path(temp) / "firstboot"
            source.mkdir()
            dest.mkdir()
            (source / LEGACY).write_bytes(original)
            (source / "companion.dll").write_bytes(b"matching firmware")
            (dest / LEGACY).write_bytes(b"previous staged image")
            (dest / "companion.dll").write_bytes(b"different firmware")
            with self.assertRaisesRegex(ValueError, "mismatched companion"):
                stage_first_boot(source, dest, "alternate")
            self.assertEqual((dest / LEGACY).read_bytes(), b"previous staged image")
            self.assertEqual((dest / "companion.dll").read_bytes(), b"different firmware")
            self.assertFalse((dest / "firstboot-transform.json").exists())


if __name__ == "__main__":
    unittest.main()
