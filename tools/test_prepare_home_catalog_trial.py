"""Disposable filesystem controls for the clone-only Home trial preparation."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from prepare_home_catalog_trial import digest


class TrialPreparationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='home-trial-', dir=Path.home() / 'AppData/Local/Temp/opencode')
        self.root = Path(self.temp.name)
        self.store, self.stage, self.comparison = (self.root / x for x in ('source-store', 'source-stage', 'comparison'))
        for path, data in {
            'source-store/priv/mms/app.db': b'original-db',
            'source-store/priv/mms/app.db-corrupt': b'original-marker',
            'source-store/priv/mms/other.db': b'neighbor-db',
            'source-store/priv/cache/cache.db-wal': b'neighbor-wal',
            'source-store/priv/user/users.json': b'{"version":1,"users":[{"id":268500997}]}',
            'source-stage/common/module': b'module',
            'firmware/default/param.json': b'game-param',
            'firmware/default/icon0.png': b'game-icon',
            'firmware/NPXS40139/param.json': b'media-param',
            'firmware/NPXS40139/icon0.png': b'media-icon',
            'comparison/comparison/candidate/app.db': b'candidate-db',
            'comparison/comparison/candidate/app.db-corrupt': b'original-marker',
        }.items():
            target = self.root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        sources = {
            'app.db': self.store / 'priv/mms/app.db',
            'app.db-corrupt': self.store / 'priv/mms/app.db-corrupt',
            'users.json': self.store / 'priv/user/users.json',
        }
        for name in ('default', 'NPXS40139'):
            for file in ('param.json', 'icon0.png'):
                sources[f'library/{name}/{file}'] = self.root / 'firmware' / name / file
        manifest = {'sha256': {k: digest(v) for k, v in sources.items()},
                    'source_paths': {k: str(v) for k, v in sources.items()}, 'users': [268500997]}
        (self.comparison / 'input-manifest.json').write_text(json.dumps(manifest))
        receipt = {'candidate_app_db_sha256': digest(self.comparison / 'comparison/candidate/app.db'),
                   'users': [268500997], 'source_hashes_unchanged': True}
        (self.comparison / 'comparison-receipt.json').write_text(json.dumps(receipt))
        (self.comparison / 'comparison.log').write_text('comparison passed; no native recovery/admission or Home claim\n')

    def tearDown(self):
        self.temp.cleanup()

    def run_trial(self, *extra):
        script = Path(__file__).with_name('prepare_home_catalog_trial.py')
        return subprocess.run([sys.executable, '-B', str(script), '--source-store', str(self.store),
                               '--source-stage', str(self.stage), '--comparison', str(self.comparison),
                               '--output', str(self.root / 'trial'), *extra], capture_output=True, text=True)

    def test_control_candidate_and_preservation(self):
        self.assertEqual(self.run_trial('--source-quiescent').returncode, 0)
        root = self.root / 'trial'
        self.assertEqual((root / 'control/sd/priv/mms/app.db').read_bytes(), b'original-db')
        self.assertEqual((root / 'candidate/sd/priv/mms/app.db').read_bytes(), b'candidate-db')
        self.assertEqual((root / 'candidate/original-app.db').read_bytes(), b'original-db')
        for name in ('control', 'candidate'):
            self.assertEqual((root / name / 'sd/priv/mms/app.db-corrupt').read_bytes(), b'original-marker')
            self.assertEqual((root / name / 'sd/priv/mms/other.db').read_bytes(), b'neighbor-db')
            self.assertEqual((root / name / 'sd/priv/cache/cache.db-wal').read_bytes(), b'neighbor-wal')
        self.assertEqual((self.store / 'priv/mms/app.db').read_bytes(), b'original-db')
        self.assertNotEqual(self.run_trial('--source-quiescent').returncode, 0)

    def test_requires_quiescence_and_matching_sources(self):
        self.assertNotEqual(self.run_trial().returncode, 0)
        (self.store / 'priv/mms/app.db-corrupt').write_bytes(b'changed')
        self.assertNotEqual(self.run_trial('--source-quiescent').returncode, 0)
        self.assertFalse((self.root / 'trial').exists())

    def test_rejects_stale_candidate_and_appbrowse_sidecar(self):
        candidate = self.comparison / 'comparison/candidate/app.db'
        candidate.write_bytes(b'changed')
        self.assertNotEqual(self.run_trial('--source-quiescent').returncode, 0)
        candidate.write_bytes(b'candidate-db')
        (self.store / 'priv/mms/app.db-wal').write_bytes(b'live')
        self.assertNotEqual(self.run_trial('--source-quiescent').returncode, 0)
        self.assertFalse((self.root / 'trial').exists())


if __name__ == '__main__':
    unittest.main()
