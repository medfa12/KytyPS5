"""Configuration precedence and early failure without firmware or guest execution."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from local_paths import configured_path


class LocalPathsTests(unittest.TestCase):
    def test_explicit_path_wins_over_environment(self):
        with tempfile.TemporaryDirectory() as temp, patch.dict(os.environ, {"KYTY_FIRMWARE_ROOT": "wrong"}):
            self.assertEqual(configured_path(temp), Path(temp).resolve())

    def test_environment_and_missing_configuration(self):
        with tempfile.TemporaryDirectory() as temp, patch.dict(os.environ, {"KYTY_FIRMWARE_ROOT": temp}):
            self.assertEqual(configured_path(), Path(temp).resolve())
        with patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(SystemExit, "KYTY_FIRMWARE_ROOT"):
                configured_path()
            with self.assertRaises(SystemExit):
                configured_path("")

    def test_staging_tools_fail_before_writing_without_configuration(self):
        tools = Path(__file__).resolve().parent
        env = {k: v for k, v in os.environ.items() if k != "KYTY_FIRMWARE_ROOT"}
        with tempfile.TemporaryDirectory() as temp:
            for script in ("prepare_shellui_managed_1360.py", "prepare_shellui_databases_1360.py"):
                result = subprocess.run([sys.executable, "-B", str(tools / script)],
                                        cwd=temp, env=env, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("KYTY_FIRMWARE_ROOT", result.stderr)
                self.assertEqual(list(Path(temp).iterdir()), [])


if __name__ == "__main__":
    unittest.main()
