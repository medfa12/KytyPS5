"""Stage the known shared-host plaintext comparison into a NEW directory.

This is explicitly a mixed/unverified plaintext experiment, not retail
decryption, signature verification, authentication, or a strict-retail run.
Only NPXS40141 is overlaid. Original firmware and source are never modified.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct

FILES = {
    'host.ps.bundle': ('f31a86d7385339156b12ae0e65da0093ca80d972e2cfcbc8cbb890dc6b7a1c89', 'ce1bef142fe8acdb448d616b65567c1b4245df15cb0a9ad7c398485a1e75e2eb'),
    'host.ps.bundle.hbc': ('ce47ad765106566203ef9d801f0bd67aa5f58f8ab3617df8c4851b875bfee04f', 'eb6c5a98be8cb01cb60d409304be3567eeabd66d8b89b0ddd78d645dcc7bc237'),
    'base_dll.ps.bundle': ('bbfce5480c222c2376d1969090d60ca3020e5737e4d640750e736f04642c9cbe', 'd4c797b851c3c6f52fb1e2ff3a2ff57cf78c01613d5aa677ca495451c943d6a3'),
    'base_dll.ps.bundle.hbc': ('acb39d6aa9d9ae9699cf8dbb5da67492e7107b8d990c8b58c23fb7001855017b', '12c7a876da06d1d01dc9c8dd559c6dd41c961f187863d637d6003e1e72dffd74'),
}


def digest(b):
    return hashlib.sha256(b).hexdigest()


def validate_pair(name, wrapped, plain, expected_wrapped, expected_plain):
    if digest(wrapped) != expected_wrapped or digest(plain) != expected_plain:
        raise ValueError(f'{name}: input hash mismatch')
    if len(wrapped) < 48 or wrapped[:8] != b'RNPSHEDR':
        raise ValueError(f'{name}: invalid container')
    offset, stored, size = struct.unpack_from('<III', wrapped, 0x1c)
    if offset > len(wrapped) or stored > len(wrapped) - offset or len(plain) != size:
        raise ValueError(f'{name}: length mismatch')
    if name.endswith('.hbc'):
        if len(plain) < 36 or plain[:8] != bytes.fromhex('c61fbc03c103191f'):
            raise ValueError(f'{name}: invalid Hermes magic')
        version = struct.unpack_from('<I', plain, 8)[0]
        file_size = struct.unpack_from('<I', plain, 32)[0]
        if version != 89 or file_size < len(plain) or file_size > 256 * 1024 * 1024:
            raise ValueError(f'{name}: unsupported Hermes header')


def prepare(firmware, source, stage):
    firmware, source, stage = firmware.resolve(), source.resolve(), stage.resolve()
    if stage.exists():
        raise ValueError('stage must not exist; never replace an existing tree')
    for protected in (firmware, source):
        if stage == protected or stage in protected.parents or protected in stage.parents:
            raise ValueError('stage overlaps firmware or plaintext source')
    title_source = source / 'NPXS40141'
    if {p.name for p in source.iterdir()} != {'NPXS40141'} or not title_source.is_dir():
        raise ValueError('source must contain only the NPXS40141 overlay directory')
    if {p.name for p in title_source.iterdir()} != set(FILES):
        raise ValueError('overlay must contain exactly the four audited files')
    original = firmware / 'system_ex_b/rnps/apps/NPXS40141'
    manifest = (original / 'manifest.json').read_bytes()
    if digest(manifest) != '470f7db6719bc522e2a8352e95c6e94eea9820d1c3834d4f77c6e8e01deb7af3':
        raise ValueError('retail host manifest hash mismatch')
    for name, (wrapped_hash, plain_hash) in FILES.items():
        validate_pair(name, (original / name).read_bytes(), (title_source / name).read_bytes(), wrapped_hash, plain_hash)
    host = (title_source / 'host.ps.bundle').read_text(encoding='utf-8')
    base = (title_source / 'base_dll.ps.bundle').read_text(encoding='utf-8')
    ids = set(re.findall(r'n\(0\)\("([^"]+)"\)', host))
    if len(ids) != 9 or not all(key in base for key in ids) or not all(
            name in host for name in ('bundleRegistryLoad', 'base_dll', 'getBundleExport', 'registerComponent', 'Main')):
        raise ValueError('host/base dependency check failed')
    spec = importlib.util.spec_from_file_location('rnps_prep', Path(__file__).with_name('prepare_shellui_managed_1360.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    # Claim the new destination atomically before the existing staging helper
    # can remove any title directory from it.
    stage.mkdir(parents=True, exist_ok=False)
    titles = module.stage_rnps_apps(firmware, stage, source)
    if titles != ['NPXS40141']:
        raise ValueError('unexpected staged titles')
    target = stage / 'rnps/apps/NPXS40141'
    for name, (_, plain_hash) in FILES.items():
        if digest((target / name).read_bytes()) != plain_hash:
            raise ValueError('staged bytes differ')
    if digest((target / 'manifest.json').read_bytes()) != digest(manifest):
        raise ValueError('retail manifest changed')
    report = {'classification': 'MIXED: supplied plaintext, NOT cryptographically verified',
              'title': 'NPXS40141', 'retail_manifest_sha256': digest(manifest),
              'plaintext_hashes': {n: h[1] for n, h in FILES.items()}, 'hermes_version': 89,
              'host_base_module_ids_found': len(ids),
              'mount': f'/system_ex/rnps/apps/NPXS40141={target}',
              'limitations': 'No signature/decryption authentication or guest execution verified.'}
    (stage / 'comparison-receipt.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--stage', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(prepare(args.firmware, args.source, args.stage), indent=2))


if __name__ == '__main__':
    main()
