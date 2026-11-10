"""Offline pinned BGLayer policy audit; never reads a process or changes firmware.

Input is the existing analysis-only BGLayer ET_DYN view, not an executable to run.
IDA/Hex-Rays identifies the setter; MethodDef/call-table and GOT #US metadata
independently identify its caller and literal. Byte checks pin the decisive edge.
"""
import argparse
import json
from pathlib import Path
import struct

import inspect_appbrowse_aot_1360 as aot

VIEW_SHA256 = '741da5eda3233a62cf74e7f3ba978e80533bfa7b458af0d2b7b6b532a17365a0'


def audit(path):
    aot.HASHES['bg-gen2-view'] = VIEW_SHA256
    image = aot.Image(path, 'bg-gen2-view')
    prefix = 'Sce.Vsh.ShellUI.BGLayer.BackgroundLayer::'
    for address, method in ((0x1c60, 'Start'), (0x3c70, 'get_EnableGen2VrRendering'),
                            (0x3c80, 'set_EnableGen2VrRendering')):
        if image.methods.get(address) != prefix + method:
            raise ValueError('MethodDef/call-table mismatch')
    for address, raw in ((0x1e23, '4c89f7be01000000'),  # rdi=this; esi=literal true
                         (0x3c74, '8a87c8000000'),    # getter field
                         (0x3cf1, '448833'),           # setter field write
                         (0x3d02, '83c903')):          # enable shared flags0/1
        expected = bytes.fromhex(raw)
        if image.read(address, len(expected)) != expected:
            raise ValueError(f'Instruction mismatch at {address:#x}')
    call = image.read(0x1e42, 5)
    if call[0] != 0xe8 or 0x1e47 + struct.unpack('<i', call[1:])[0] != 0x3c80:
        raise ValueError('Start -> setter edge changed')
    if image.patch(0xcd088) != repr('Enable Gen2Vr Rendering: {0}'):
        raise ValueError('GOT #US log literal mismatch')
    return dict(view_sha256=VIEW_SHA256, caller='BackgroundLayer.Start', caller_rva='0x1c60',
                call_rva='0x1e42', setter_rva='0x3c80', argument='literal true',
                owner_field='0xc8', log='Enable Gen2Vr Rendering: {0}',
                conclusion='This startup call supplies policy true, not a headset-status result.',
                scope='Static edge only; does not establish particle dispatch or active VR output.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('analysis_view', type=Path)
    args = parser.parse_args()
    print(json.dumps(audit(args.analysis_view), indent=2))
