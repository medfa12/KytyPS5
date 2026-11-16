"""Prepare two explicitly mixed RNPS app overlays in a NEW isolated stage.

Plaintext is comparison-tree input, not authenticated retail decryption.
No guest execution, user state, setup state, or authentication is produced.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile

from prepare_retail_host_comparison import digest, validate_pair

TITLES = {
    'NPXS40008': ('rnps-settings', '2af65f7e817dc2ff24f98af6fe3430965c2b9e7ddda4350a278a7464bbc6beea',
                  '1cb21ba1f3f2070743802a5dc9762a3ee5f0d9041597e37f1dc3702679af4fad',
                  'f6e0b33e66df1847cf20a1661bfc3c3d7eeb606f3456f31ce88780438aa72718'),
    'NPXS40011': ('rnps-notification-overlay', '5ce7d1326383e08eb1a2e800f3cec4a2f0bf162ea4780db4a9ab4fdf217dca6b',
                  'a51a2d05b6fb9ae618208e17a450a38a87ae51c0913199ae046e316eba7a9dc3',
                  'f49b5c2e7e208e5a1acbbeed2f429343a68451c91a3133b8436bea6098985bcd'),
}
GENERATED_SIDECARS = ('application.ps.hbc', 'application.ps.js', 'decompiler.stderr.log', 'decompiler.stdout.log')


def prepare(firmware, comparison_apps, stage):
    firmware, comparison_apps, stage = firmware.resolve(), comparison_apps.resolve(), stage.resolve()
    if stage.exists():
        raise ValueError('stage must not exist; never replace an existing tree')
    for protected in (firmware, comparison_apps):
        if stage == protected or stage in protected.parents or protected in stage.parents:
            raise ValueError('stage overlaps an input tree')
    originals = firmware / 'system_ex_b/rnps/apps'
    records = {}
    for title, (app_name, manifest_hash, wrapped_hash, plaintext_hash) in TITLES.items():
        manifest = (originals / title / 'manifest.json').read_bytes()
        comparison_manifest = (comparison_apps / title / 'manifest.json').read_bytes()
        identity = json.loads(manifest)
        if digest(manifest) != manifest_hash or digest(comparison_manifest) != manifest_hash or \
           identity.get('titleId') != title or identity.get('applicationName') != app_name or not identity.get('twinTurbo'):
            raise ValueError(f'{title}: application manifest mismatch')
        wrapped = (originals / title / 'application.ps.bundle').read_bytes()
        plaintext = (comparison_apps / title / 'application.ps.bundle').read_bytes()
        validate_pair('application.ps.hbc', wrapped, plaintext, wrapped_hash, plaintext_hash)
        if b'base_dll' not in plaintext or b'bundleRegistryLoad' not in plaintext:
            raise ValueError(f'{title}: missing twin-turbo dependency markers')
        records[title] = {'application': app_name, 'manifest_sha256': manifest_hash,
                          'retail_container_sha256': wrapped_hash, 'comparison_plaintext_sha256': plaintext_hash,
                          'declared_plaintext_bytes': len(plaintext), 'hermes_version': 89}
    # Claim the destination atomically; never run the staging helper over a
    # pre-existing title directory. Temporary inputs hold only explicit files.
    stage.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location('rnps_prep', Path(__file__).with_name('prepare_shellui_managed_1360.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    with tempfile.TemporaryDirectory(dir=stage.parent, prefix='rnps-app-input-') as temp:
        source = Path(temp)
        for title in TITLES:
            (source / title).mkdir()
            shutil.copyfile(comparison_apps / title / 'application.ps.bundle', source / title / 'application.ps.bundle')
        if module.stage_rnps_apps(firmware, stage, source) != sorted(TITLES):
            raise ValueError('unexpected staged title set')
    mounts = []
    for title, (_, manifest_hash, _, plaintext_hash) in TITLES.items():
        target = stage / 'rnps/apps' / title
        # These are generated sidecars copied into OUR NEW stage, not retail
        # runtime inputs. Do not remove or change them in the source tree.
        for name in GENERATED_SIDECARS:
            (target / name).unlink(missing_ok=True)
        if digest((target / 'manifest.json').read_bytes()) != manifest_hash or \
           digest((target / 'application.ps.bundle').read_bytes()) != plaintext_hash:
            raise ValueError(f'{title}: staged bytes changed')
        mounts.append(f'/system_ex/rnps/apps/{title}={target}')
    report = {'classification': 'MIXED: comparison-tree plaintext, NOT cryptographically verified',
              'titles': records, 'mounts': mounts,
              'limitations': 'No signature/decryption authentication, JS execution, Home or PSN success verified.'}
    (stage / 'comparison-receipt.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware', type=Path, required=True)
    parser.add_argument('--comparison-apps', type=Path, required=True)
    parser.add_argument('--stage', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(prepare(args.firmware, args.comparison_apps, args.stage), indent=2))


if __name__ == '__main__':
    main()
