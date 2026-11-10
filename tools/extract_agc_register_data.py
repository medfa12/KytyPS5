"""Extract AGC base arrays from a user's matched decrypted module; no payloads bundled."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


SOURCE_BYTES = 382976
SOURCE_SHA256 = 'a12b0f396a6464800a68006d6d64e00349a7b37fc3c1c08b6236e8ca564cbd6d'
# File offsets and byte lengths, not register values. Rows are tbl0/1/2/3/types;
# absent tables have no extraction range. The public tables have no tbl3.
RANGES = {
    'internal': {
        0: [(0x3c018, 8), (0x3c020, 96), None, (0x3c100, 32), (0x20010, 180)],
        4: [(0x41510, 16), (0x41520, 120), None, (0x3c100, 32), (0x226d0, 228)],
        5: [(0x430a0, 24), (0x41520, 120), None, (0x3de08, 48), (0x23430, 252)],
        7: [(0x44da8, 32), (0x41520, 120), None, (0x3de08, 48), (0x24210, 264)],
        8: [(0x44da8, 32), (0x41520, 120), None, (0x3de08, 48), (0x24210, 264)],
        9: [(0x44da8, 32), (0x484c0, 112), (0x3dd30, 8), (0x3de08, 48), (0x25990, 264)],
        10: [(0x4a030, 72), (0x41520, 120), (0x3dd30, 8), (0x3de08, 48), (0x26580, 336)],
        11: [(0x3dc90, 64), (0x3dcd0, 96), (0x3dd30, 8), (0x3de08, 48), (0x20d50, 288)],
    },
    'public': {
        0: [(0x3f9a0, 4184), (0x3d178, 1288), (0x3d680, 344), None, (0x20580, 1800)],
        4: [(0x41660, 3904), (0x425a0, 1264), (0x3d680, 344), None, (0x22c70, 1800)],
        5: [(0x43218, 3928), (0x44170, 1272), (0x44668, 600), None, (0x23a10, 1872)],
        7: [(0x44f28, 3912), (0x44170, 1272), (0x46368, 440), None, (0x24720, 1524)],
        8: [(0x46aa8, 3912), (0x44170, 1272), (0x46368, 440), None, (0x24720, 1524)],
        9: [(0x48628, 3912), (0x49570, 1280), (0x46368, 440), None, (0x25ea0, 1524)],
        10: [(0x4a218, 3856), (0x44170, 1272), (0x46368, 440), None, (0x26ad0, 1536)],
        11: [(0x3de38, 3904), (0x3ed78, 1296), (0x3f288, 448), None, (0x212c0, 1644)],
    },
}


def extract(module):
    if module.stat().st_size != SOURCE_BYTES:
        raise ValueError('Unsupported AGC module size; use the matching decrypted ELF')
    data = module.read_bytes()
    if hashlib.sha256(data).hexdigest() != SOURCE_SHA256:
        raise ValueError('Unsupported AGC module SHA-256; no extraction performed')
    arrays = {}
    for family, versions in RANGES.items():
        for version, tables in versions.items():
            for table, region in enumerate(tables):
                if region is None:
                    continue
                offset, length = region
                if offset < 0 or length <= 0 or length % 4 or offset + length > len(data):
                    raise ValueError('Invalid extraction range')
                suffix = 'types' if table == 4 else f'tbl{table}_regs'
                name = f'g_agc_{family}_reg_defaults_v{version}_{suffix}'
                arrays[name] = list(struct.unpack_from('<' + 'I' * (length // 4), data, offset))
    return {'schema': 'kyty-agc-base-arrays-v1', 'source_sha256': SOURCE_SHA256,
            'arrays': arrays}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--output', type=Path, help='New external JSON file; existing files are preserved')
    parser.add_argument('--check', action='store_true', help='Verify/extract in memory without writing')
    args = parser.parse_args()
    if not args.check and args.output is None:
        parser.error('provide --output or --check')
    try:
        module = args.module.resolve()
        if args.output is not None:
            output = args.output.resolve()
            installation = Path(__file__).resolve().parent.parent
            if output == module or installation == output or installation in output.parents:
                raise ValueError('Output must be outside the installation and must not replace the module')
        result = extract(module)
        if not args.check:
            # The caller supplies an existing external directory. Never overwrite
            # firmware or a previous extraction, including through a symlink.
            with output.open('x', encoding='utf-8', newline='\n') as file:
                json.dump(result, file, separators=(',', ':'))
                file.write('\n')
        print(json.dumps({'verified': True, 'arrays': len(result['arrays']),
                          'source_sha256': SOURCE_SHA256, 'written': not args.check}))
        return 0
    except (OSError, ValueError) as exc:
        print(json.dumps({'verified': False, 'error': str(exc)}))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
