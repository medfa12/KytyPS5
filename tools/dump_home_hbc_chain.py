"""Hash-pinned offline Hermes HBC function dump (not an IDA/native trace)."""
import argparse
import hashlib
from io import BytesIO
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    parser.add_argument('--sha256', required=True)
    parser.add_argument('--decoder', type=Path, required=True)
    parser.add_argument('functions', type=int, nargs='+')
    args = parser.parse_args()
    data = args.bundle.read_bytes()
    if hashlib.sha256(data).hexdigest() != args.sha256:
        raise ValueError('HBC source hash mismatch')
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(args.decoder.resolve()))
    from hermes_dec.parsers.hbc_file_parser import HBCReader
    from hermes_dec.parsers.hbc_bytecode_parser import parse_hbc_bytecode
    reader = HBCReader()
    reader.read_whole_file(BytesIO(data))
    if reader.header.version != 89:
        raise ValueError('unsupported HBC version')
    print('SHA256', args.sha256, 'HBC version', reader.header.version)
    for fid in args.functions:
        header = reader.function_headers[fid]
        print(f'\nFunction #{fid} file+0x{header.offset:x} size=0x{header.bytecodeSizeInBytes:x}')
        for handler in reader.function_id_to_exc_handlers.get(fid, []):
            print(f'catch [0x{handler.start:x},0x{handler.end:x}) -> 0x{handler.target:x}')
        for instruction in parse_hbc_bytecode(header, reader):
            print(instruction)
    if hashlib.sha256(args.bundle.read_bytes()).hexdigest() != args.sha256:
        raise RuntimeError('HBC input changed during analysis')


if __name__ == '__main__':
    main()
