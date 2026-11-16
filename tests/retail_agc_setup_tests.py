"""Installed AGC staging contract; requires the user's external decrypted module."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

package = Path(sys.argv.pop(1)).resolve()
module = Path(sys.argv.pop(1)).resolve()
sys.dont_write_bytecode = True
sys.path.insert(0, str(package / 'tools'))
spec = importlib.util.spec_from_file_location('setup', package / 'tools/setup_retail.py')
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)


class AgcSetup(unittest.TestCase):
    def test_real_extraction_and_reuse(self):
        with tempfile.TemporaryDirectory(prefix='ag', dir=os.environ['TEMP']) as tmp:
            root = Path(tmp)
            fw = root / 'fw'
            image = fw / 'system_ex_b/app/NPXS40087/eboot.bin'
            image.parent.mkdir(parents=True)
            image.write_bytes(b'synthetic launcher; profile detector mocked')
            (fw / 'system_b').mkdir()
            (fw / 'system_ex_b/vsh_asset').mkdir()
            (fw / 'system_ex_b/rnps/apps/NPXS40141').mkdir(parents=True)
            rnps = root / 'rnps'
            (rnps / 'NPXS40141').mkdir(parents=True)
            url = root / 'url.json'
            url.write_text('{}')
            data = root / 'data'
            argv = ['setup', '--firmware', str(fw), '--data', str(data),
                    '--url-config', str(url), '--rnps-source', str(rnps),
                    '--agc-module', str(module)]
            expected = (json.dumps(setup.extract_agc(module), separators=(',', ':')) + '\n').encode()
            detected = subprocess.CompletedProcess([], 0, 'active\n', '')
            with patch.object(setup.subprocess, 'run', return_value=detected), \
                 patch.object(setup.subprocess, 'call', return_value=0) as launch:
                with patch.object(sys, 'argv', argv + ['--check']):
                    self.assertEqual(setup.main(), 0)
                    self.assertFalse(data.exists())
                    launch.assert_not_called()
                for mode in ([], ['--launch']):
                    with patch.object(sys, 'argv', argv + mode), \
                         patch.dict(os.environ, {'KYTY_AGC_REGISTER_DATA': 'unrelated.json'}):
                        self.assertEqual(setup.main(), 0)
                    output = data / 'agc-register-data.json'
                    self.assertEqual(output.read_bytes(), expected)
                    command = launch.call_args.args[0]
                    self.assertEqual(command[command.index('-AgcRegisterData') + 1], str(output))
                    self.assertNotIn('KYTY_AGC_REGISTER_DATA', launch.call_args.kwargs['env'])
                output.write_bytes(b'preserve unexpected existing data')
                launch.reset_mock()
                with patch.object(sys, 'argv', argv):
                    self.assertEqual(setup.main(), 1)
                launch.assert_not_called()
                self.assertEqual(output.read_bytes(), b'preserve unexpected existing data')
                # A missing module is a prerequisite error even in check mode.
                with patch.object(sys, 'argv', argv + ['--agc-module', str(root / 'missing.elf'), '--check']):
                    self.assertEqual(setup.main(), 1)
                launch.assert_not_called()


unittest.main()
