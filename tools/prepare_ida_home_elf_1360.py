"""Create a private IDA-analysis ELF view, never an executable firmware patch.

IDA 9.3's autonomous ELF loader rejects Sony e_type 0xfe18. This tool changes
ONLY that two-byte header field to ET_DYN (or Sony executable 0xfe10 to ET_EXEC)
on an exclusively created copy, and
requires the caller's exact original SHA256. Code, segments and firmware remain
unchanged. Result is for static analysis, not guest loading or execution.
"""
import argparse
import hashlib
from pathlib import Path
import struct


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--sha256', required=True)
    a = p.parse_args()
    data = a.source.read_bytes()
    original = hashlib.sha256(data).hexdigest()
    kind = struct.unpack_from('<H', data, 16)[0] if len(data) >= 18 else None
    if original != a.sha256 or data[:6] != b'\x7fELF\x02\x01' or kind not in (0xfe18, 0xfe10):
        raise ValueError('wrong hash or non-Sony ELF64 little-endian e_type')
    analysis = bytearray(data)
    target = 3 if kind == 0xfe18 else 2
    struct.pack_into('<H', analysis, 16, target)
    if analysis[:16] != data[:16] or analysis[18:] != data[18:]:
        raise AssertionError('unexpected non-header modification')
    with a.output.open('xb') as f:
        f.write(analysis)
    if hashlib.sha256(a.source.read_bytes()).hexdigest() != original:
        raise RuntimeError('source changed during copy')
    print('source_sha256', original)
    print('analysis_sha256', hashlib.sha256(analysis).hexdigest())
    print(f'analysis-only mutation: ELF e_type offset 0x10, {kind:#x} -> {target}; every other byte identical')


if __name__ == '__main__':
    main()
