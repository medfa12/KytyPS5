"""Offline execution of pinned Home intro classification HBC with atom doubles.

Reports branch selection, not animation playback, native gates or live state.
"""
import argparse
import hashlib
from io import BytesIO
import json
from pathlib import Path
import sys

from probe_home_first_render_1360 import Probe, SHA256


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    parser.add_argument('--decoder', type=Path, required=True)
    args = parser.parse_args()
    data = args.bundle.read_bytes()
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise ValueError('Home HBC hash mismatch')
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(args.decoder.resolve()))
    from hermes_dec.parsers.hbc_file_parser import HBCReader
    from hermes_dec.parsers.hbc_bytecode_parser import parse_hbc_bytecode
    from hermes_dec.parsers.serialized_literal_parser import unpack_slp_array
    reader = HBCReader()
    reader.read_whole_file(BytesIO(data))
    if reader.header.version != 89:
        raise ValueError('unsupported HBC version')
    probe = Probe(reader, parse_hbc_bytecode, unpack_slp_array)
    cases = [
        ({'intro': 'login', 'bootCondition': 'login'}, 'initialLoad'),
        ({'intro': 'login', 'bootCondition': 'autoLogin'}, 'autoLogin'),
        ({'intro': 'login', 'bootCondition': 'autoLogin', 'postAutoLogin': True}, 'appRestart'),
        ({'intro': 'hidden', 'bootCondition': 'login'}, 'autoLogin'),
        ({'bootCondition': 'switchUser'}, 'switchUser'),
        ({'intro': 'login', 'lastFocusTitleId': 'NPXS40002'}, 'switchUser'),
        ({'intro': 'login'}, 'initialLoad'),
        ({}, 'initialLoad'),
    ]
    results = []
    atom = object()
    for query, expected in cases:
        def get(value):
            if value is not atom:
                raise ValueError('unexpected atom dependency')
            return {'query': query}
        result = probe.call(6183, env=[[{'default': atom}]], args=[get])
        if result != expected:
            raise AssertionError((query, result, expected))
        results.append({'query': query, 'classification': result})
    if hashlib.sha256(args.bundle.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError('input changed during analysis')
    print(json.dumps({'sha256': SHA256, 'function': 6183,
                      'scope': 'actual HBC subset with atom test double; no live animation',
                      'cases': results}, indent=2))


if __name__ == '__main__':
    main()
