"""Synthetic red 64x48 keyframe; normal tests require no encoder or files."""
from pathlib import Path
import hashlib
import re
import shutil
import subprocess

root = Path(__file__).resolve().parent
ffmpeg = shutil.which('ffmpeg')
lines = ['// Synthetic red frame. Regenerate with generate.py.', '#pragma once',
         '#include <array>', '#include <cstdint>', 'namespace EsvmNalFixture {']
for codec, encoder, options in [('Avc', 'libx264', ['-preset', 'ultrafast', '-tune', 'zerolatency']),
                                ('Hevc', 'libx265', ['-x265-params', 'pools=none:frame-threads=1:log-level=error']),
                                ('Hevc10', 'libx265', ['-x265-params', 'pools=none:frame-threads=1:log-level=error:colorprim=bt709:transfer=bt709:colormatrix=bt709']),
                                ('HevcPq', 'libx265', ['-x265-params', 'pools=none:frame-threads=1:log-level=error:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc'])]:
    data = subprocess.check_output([ffmpeg, '-hide_banner', '-loglevel', 'error', '-f', 'lavfi',
        '-i', 'color=c=red:s=64x48:r=1', '-frames:v', '1', '-pix_fmt', 'yuv420p10le' if codec in ('Hevc10', 'HevcPq') else 'yuv420p', '-c:v', encoder,
        *(['-vf', 'scale=in_color_matrix=bt601:out_color_matrix=bt709'] if codec == 'Hevc10' else []),
        *options, '-threads', '1', '-f', 'h264' if codec == 'Avc' else 'hevc', 'pipe:1'])
    nals = [x for x in re.split(b'\x00\x00(?:\x00)?\x01', data) if x]
    for nal in nals:
        kind = (nal[0] & 31) if codec == 'Avc' else (nal[0] >> 1) & 63
        name = ({7: 'Sps', 8: 'Pps', 5: 'Idr'} if codec == 'Avc' else {32: 'Vps', 33: 'Sps', 34: 'Pps', 20: 'Idr'}).get(kind)
        if not name:
            continue
        lines.append(f'inline constexpr std::array<uint8_t, {len(nal)}> {codec}{name} {{')
        for offset in range(0, len(nal), 16):
            lines.append('\t' + ', '.join(f'0x{b:02x}' for b in nal[offset:offset + 16]) + ',')
        lines.append('};')
        print(codec + name, len(nal), hashlib.sha256(nal).hexdigest())
lines.extend(['}', ''])
(root / 'packets.h').write_text('\n'.join(lines), encoding='utf-8')
