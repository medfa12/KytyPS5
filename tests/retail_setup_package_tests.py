"""Installed setup dispatch contract; synthetic inputs, no firmware boot."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

package = Path(sys.argv.pop(1)).resolve()
sys.dont_write_bytecode = True
sys.path.insert(0, str(package / 'tools'))
spec = importlib.util.spec_from_file_location('setup', package / 'tools/setup_retail.py')
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)


class InstalledDispatch(unittest.TestCase):
    def test_prepare_check_and_launch(self):
        # Keep native IPC paths short. Test output belongs beside this external
        # test snapshot, never under the source checkout or installed package.
        with tempfile.TemporaryDirectory(prefix='rs', dir=os.environ['TEMP']) as tmp:
            root = Path(tmp)
            fw = root / 'fw'
            image = fw / 'system_ex_b/app/NPXS40087/eboot.bin'
            image.parent.mkdir(parents=True)
            image.write_bytes(b'synthetic launcher: detector is mocked')
            (fw / 'system_b').mkdir()
            (fw / 'system_ex_b/vsh_asset').mkdir()
            (fw / 'system_ex_b/rnps/apps/NPXS40141').mkdir(parents=True)
            rnps = root / 'rnps'
            (rnps / 'NPXS40141').mkdir(parents=True)
            url = root / 'url.json'
            url.write_text('{}')
            data = root / 'data'
            argv = ['setup', '--firmware', str(fw), '--data', str(data),
                    '--url-config', str(url), '--rnps-source', str(rnps)]
            detected = subprocess.CompletedProcess([], 0, 'active\n', '')
            with patch.object(setup.subprocess, 'run', return_value=detected), \
                 patch.object(setup, 'extract_agc', return_value={'synthetic': True}), \
                 patch.object(setup.subprocess, 'call', return_value=0) as launch:
                with patch.object(sys, 'argv', argv + ['--check']):
                    self.assertEqual(setup.main(), 0)
                    self.assertFalse(data.exists())
                    launch.assert_not_called()
                for mode, expected in (([], '-PrepareOnly'), (['--launch'], '-Visible')):
                    with patch.object(sys, 'argv', argv + mode), \
                         patch.dict(os.environ, {'KYTY_NOTIFICATION_REFERENCE': 'unrelated.db'}):
                        self.assertEqual(setup.main(), 0)
                    command = launch.call_args.args[0]
                    self.assertIn(expected, command)
                    self.assertEqual(command[command.index('-AgcRegisterData') + 1],
                                     str(data / 'agc-register-data.json'))
                    self.assertEqual((data / 'agc-register-data.json').read_text(), '{"synthetic":true}\n')
                    for option, value in (('-RuntimeRoot', data), ('-WorkDir', data / 'work'),
                                          ('-ManagedStage', data / 'managed'),
                                          ('-SystemDataPath', data / 'system')):
                        self.assertEqual(command[command.index(option) + 1], str(value))
                    self.assertNotIn('KYTY_NOTIFICATION_REFERENCE', launch.call_args.kwargs['env'])
                    self.assertEqual(launch.call_args.kwargs['cwd'], data / 'work')


unittest.main()
