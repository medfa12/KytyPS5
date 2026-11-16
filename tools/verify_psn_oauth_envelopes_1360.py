"""Offline signature verification of regular-13.60 ShellCore OAuth envelopes.

Uses the embedded public key and the signed-data layout recovered from the
matching kernel. Reports structural/signature validity, not secret recovery,
SELF admission, device authentication, or acceptance by PSN.
Requires cryptography and pyelftools.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from elftools.elf.elffile import ELFFile

CORE_SHA256 = '6870113e176333d95f1241864af35c20ac7115c23948e002da0ebaec4819250a'


def verify_envelope(blob, key):
    if len(blob) < 0x1d1:
        raise ValueError('Truncated envelope')
    version, kind = struct.unpack_from('<II', blob, 4)
    length, = struct.unpack_from('<Q', blob, 0x10)
    if (blob[:4] != bytes.fromhex('cccb7351') or version != 1 or kind != 0x28
            or length + 0x1d0 != len(blob)):
        raise ValueError('Unexpected regular-13.60 OAuth envelope header')
    signed = blob[:0x50] + bytes(0x180) + blob[0x1d0:]
    key.verify(blob[0x50:0x1d0], signed,
               padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=padding.PSS.AUTO),
               hashes.SHA256())
    return {'version': version, 'type': kind, 'payload_bytes': length,
            'signature': 'RSA-3072/PSS/SHA-256 verified',
            'envelope_sha256': hashlib.sha256(blob).hexdigest()}


def inspect(path):
    image = path.read_bytes()
    digest = hashlib.sha256(image).hexdigest()
    if digest != CORE_SHA256:
        raise ValueError('ShellCore source hash mismatch; offsets apply only to audited regular 13.60')
    elf = ELFFile(io.BytesIO(image))
    loads = [dict(s.header) for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']

    def data(va, size):
        for s in loads:
            delta = va - s['p_vaddr']
            if 0 <= delta and delta + size <= s['p_filesz']:
                start = s['p_offset'] + delta
                result = image[start:start + size]
                if len(result) == size:
                    return result
        raise ValueError(f'Extent is not file-backed: {va:#x}+{size:#x}')

    public = data(0x1f531f0, 0x300)
    key = rsa.RSAPublicNumbers(int.from_bytes(public[0x180:], 'big'),
                              int.from_bytes(public[:0x180], 'big')).public_key()
    results = []
    for va in (0x1f525f0, 0x1f527d0):
        results.append({'rva': hex(va), **verify_envelope(data(va, 0x1e0), key)})
    return {'image': str(path.resolve()), 'sha256': digest, 'envelopes': results,
            'plaintext_recovered': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('core', type=Path, help='Audited regular-13.60 SceShellCore.elf')
    parser.add_argument('--output', type=Path, help='Optional JSON report path')
    args = parser.parse_args()
    text = json.dumps(inspect(args.core), indent=2) + '\n'
    if args.output:
        args.output.write_text(text, encoding='utf-8')
    print(text, end='')


if __name__ == '__main__':
    main()
