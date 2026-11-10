"""Exercise clone relocation against real LocalUserStorage creation and recovery."""
import os
from pathlib import Path
import json
import shutil
import subprocess
import sys
import tempfile
import unittest

from prepare_home_catalog_trial import apply_receipt_relocation, digest, inventory, plan_receipt_relocation


class CloneReceiptTests(unittest.TestCase):
    def setUp(self):
        executable = os.environ.get('KYTY_STORAGE_RECOVER_PROBE')
        if not executable:
            self.skipTest('set KYTY_STORAGE_RECOVER_PROBE to standalone real-storage probe')
        self.exe = str(Path(executable).resolve(strict=True))
        self.tmp = tempfile.TemporaryDirectory(prefix='home-user-clone-', dir=Path.home() / 'AppData/Local/Temp/opencode')
        self.root = Path(self.tmp.name)
        self.source = self.root / 'source'
        self.clone = self.root / 'clone'
        self.assertEqual(self.probe('make', self.source).returncode, 0)
        shutil.copytree(self.source, self.clone)
        self.baseline = inventory(self.source, False)

    def tearDown(self):
        if hasattr(self, 'tmp'):
            self.tmp.cleanup()

    def probe(self, action, root):
        return subprocess.run([self.exe, action, str(root)], capture_output=True, text=True)

    def test_real_recovery_before_and_after_relocation(self):
        before = self.probe('recover', self.clone)
        self.assertEqual(before.returncode, 4, before.stdout + before.stderr)
        plans = plan_receipt_relocation(self.source, self.clone)
        self.assertEqual(len(plans), 3)
        result = apply_receipt_relocation(self.clone, self.root / 'archived', plans)
        self.assertEqual(len(result), 3)
        for name, raw, rewritten in plans:
            self.assertEqual((self.root / 'archived' / name).read_bytes(), raw)
            self.assertEqual((self.clone / 'priv/user/local-user-storage' / name).read_bytes(), rewritten)
        recovered = self.probe('recover', self.clone)
        self.assertEqual(recovered.returncode, 0, recovered.stdout + recovered.stderr)
        for part in ('user/home', 'priv/home', 'eap/rodata/home'):
            self.assertTrue((self.clone / part / '10010004/.kyty-local-user-owner').is_file())
            self.assertTrue((self.clone / part / '10010005/.kyty-local-user-owner').is_file())
            self.assertFalse((self.clone / part / '20010000').exists())
            self.assertTrue((self.source / part / '20010000/.kyty-local-user-owner').is_file())
        self.assertFalse((self.clone / 'priv/user/local-user-storage/user-20010000.receipt').exists())
        self.assertEqual(inventory(self.source, False), self.baseline)

    def test_reject_mismatched_owner_without_rewriting(self):
        marker = self.clone / 'priv/home/10010004/.kyty-local-user-owner'
        marker.write_bytes(b'foreign-owner\n')
        receipt = self.clone / 'priv/user/local-user-storage/user-10010004.receipt'
        original = receipt.read_bytes()
        with self.assertRaisesRegex(ValueError, 'owner mismatch'):
            plan_receipt_relocation(self.source, self.clone)
        self.assertEqual(receipt.read_bytes(), original)
        self.assertEqual(inventory(self.source, False), self.baseline)

    def test_reject_foreign_paths_and_bad_tokens(self):
        source = self.source / 'priv/user/local-user-storage/user-10010004.receipt'
        clone = self.clone / 'priv/user/local-user-storage/user-10010004.receipt'
        original = source.read_bytes()
        foreign = original.replace(self.source.as_posix().encode(), (self.root / 'foreign').as_posix().encode())
        self.assertNotEqual(foreign, original)
        source.write_bytes(foreign); clone.write_bytes(foreign)
        with self.assertRaisesRegex(ValueError, 'foreign'):
            plan_receipt_relocation(self.source, self.clone)
        # A syntactically valid changed token must still match every owner marker.
        lines = original.splitlines(keepends=True)
        parts = lines[1].split(); parts[1] = b'0' * 32
        lines[1] = b' '.join(parts) + b'\n'
        bad_token = b''.join(lines)
        source.write_bytes(bad_token); clone.write_bytes(bad_token)
        with self.assertRaisesRegex(ValueError, 'owner mismatch'):
            plan_receipt_relocation(self.source, self.clone)
        source.write_bytes(original)
        self.assertEqual(inventory(self.source, False), self.baseline)

    def test_reject_malformed_receipt_and_preserve_inputs(self):
        source = self.source / 'priv/user/local-user-storage/user-10010004.receipt'
        clone = self.clone / 'priv/user/local-user-storage/user-10010004.receipt'
        original = source.read_bytes()
        malformed = original.replace(b'KYTY-LOCAL-USER-1', b'KYTY-LOCAL-USER-X', 1)
        source.write_bytes(malformed); clone.write_bytes(malformed)
        with self.assertRaisesRegex(ValueError, 'invalid'):
            plan_receipt_relocation(self.source, self.clone)
        self.assertEqual(clone.read_bytes(), malformed)
        source.write_bytes(original)
        self.assertEqual(inventory(self.source, False), self.baseline)

    def test_interrupted_destroy_and_upgrade_keep_intent(self):
        self.assertEqual(self.probe('destroy-intent', self.source).returncode, 0)
        self.assertEqual(self.probe('upgrade-intent', self.source).returncode, 0)
        self.assertEqual(self.probe('create-intent', self.source).returncode, 0)
        shutil.rmtree(self.clone)  # This test owns only this fresh fixture clone.
        shutil.copytree(self.source, self.clone)
        before = inventory(self.source, False)
        self.assertEqual(self.probe('recover', self.clone).returncode, 4)
        plans = plan_receipt_relocation(self.source, self.clone)
        operations = {name: raw.splitlines()[1].split()[-1] for name, raw, _ in plans}
        self.assertEqual(operations['user-10010004.receipt'], b'D')
        self.assertEqual(operations['user-10010006.receipt'], b'U')
        self.assertEqual(operations['user-10010007.receipt'], b'C')
        apply_receipt_relocation(self.clone, self.root / 'archived', plans)
        result = self.probe('recover', self.clone)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for root in ('user/home', 'priv/home', 'eap/rodata/home'):
            self.assertTrue((self.clone / root / '10010004').is_dir())
            self.assertFalse((self.clone / root / '10010006').exists())
            self.assertFalse((self.clone / root / '10010007').exists())
        self.assertEqual(inventory(self.source, False), before)

    def test_actual_preparer_relocates_both_clones(self):
        for path, value in {'priv/mms/app.db': b'control', 'priv/mms/app.db-corrupt': b'marker',
                            'priv/user/users.json': b'{"version":1,"users":[{"id":268500997}]}'}.items():
            target = self.source / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(value)
        stage = self.root / 'stage'; stage.mkdir(); (stage / 'module').write_bytes(b'stage')
        cmp = self.root / 'comparison'; (cmp / 'comparison/candidate').mkdir(parents=True)
        (cmp / 'comparison/candidate/app.db').write_bytes(b'candidate')
        (cmp / 'comparison/candidate/app.db-corrupt').write_bytes(b'marker')
        paths = {'app.db': self.source / 'priv/mms/app.db', 'app.db-corrupt': self.source / 'priv/mms/app.db-corrupt',
                 'users.json': self.source / 'priv/user/users.json'}
        manifest = {'sha256': {k: digest(v) for k, v in paths.items()}, 'source_paths': {k: str(v) for k, v in paths.items()},
                    'users': [268500997]}
        (cmp / 'input-manifest.json').write_text(json.dumps(manifest))
        (cmp / 'comparison-receipt.json').write_text(json.dumps({'candidate_app_db_sha256': digest(cmp / 'comparison/candidate/app.db'),
                                                                  'users': [268500997], 'source_hashes_unchanged': True}))
        (cmp / 'comparison.log').write_text('comparison passed; no native recovery/admission or Home claim\n')
        before = inventory(self.source, False)
        script = Path(__file__).with_name('prepare_home_catalog_trial.py')
        trial = self.root / 'trial'
        result = subprocess.run([sys.executable, '-B', str(script), '--source-store', str(self.source), '--source-stage', str(stage),
                                 '--comparison', str(cmp), '--output', str(trial), '--source-quiescent'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        out = json.loads((trial / 'trial-manifest.json').read_text())
        for label in ('control', 'candidate'):
            self.assertEqual(len(out['receipt_relocation'][label]), 3)
            self.assertTrue((trial / label / 'original-local-user-receipts/user-10010004.receipt').exists())
            self.assertEqual((trial / label / 'sd/priv/mms/app.db-corrupt').read_bytes(), b'marker')
            recovered = self.probe('recover', trial / label / 'sd')
            self.assertEqual(recovered.returncode, 0, recovered.stdout + recovered.stderr)
        self.assertEqual(inventory(self.source, False), before)


if __name__ == '__main__':
    unittest.main()
