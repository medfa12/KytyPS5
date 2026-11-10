"""Pinned original-native particle job creation gate, backed by actual IDA.

Read-only static audit; these conditions have NOT been observed live.
"""
import argparse
import json
from pathlib import Path
import struct
from audit_home_hermes_call_1360 import Image
from audit_home_native_registration_1360 import SHA256

CHECKS = {
    0xe7fde: '80bf20070000010f85000100004883bba8070000000f85f2000000',
    0xe7ffe: 'c683a007000000',
    0xe801e: '488d05cb1d0801',
    0xe802a: '49890649895e38',
    0xe80a7: '4c89b3a8070000',
    0xe80e3: '4c89e7e8e5de0600',
}


def audit_image(image):
    for address, expected in CHECKS.items():
        raw = bytes.fromhex(expected)
        if image.read(address, len(raw)) != raw:
            raise ValueError(f'particle_job_instruction_{address:x}')
    return dict(sha256=SHA256, function_rva='0xe7fb0', gate_rva='0xe7fde',
                necessary_creation_conditions=['owner byte +0x720 == 1', 'owner job pointer +0x7a8 == 0'],
                otherwise='skip job construction and the call at +0xe80e6 in this invocation',
                created_job_vtable='0x1169df0', created_job_owner_offset='0x38',
                job_reference_call_rva='0xe80e6', job_reference_target='0x155fd0',
                call_argument='RDI = owner + 0x7a8',
                scope='Static creation/submission edge only; target scheduling and live gate values unknown.')


def audit(path):
    image = Image(path, SHA256)
    rel = {a: v for a, info, v in struct.iter_unpack('<QQq', image.read(image.tags[7], image.tags[8])) if info == 8}
    if rel.get(0x1169d98) != 0xe7fb0 or rel.get(0x1169e08) != 0xeb6e0:
        raise ValueError('particle_job_vtable_registration')
    return audit_image(image)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original_shell', type=Path)
    args = parser.parse_args()
    print(json.dumps(audit(args.original_shell), indent=2))
