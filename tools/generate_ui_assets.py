"""Generate original geometric UI icons and a synthetic two-note notification.

No external artwork, samples, firmware or fonts are read. Outputs are project
assets under the repository's GPL-2.0 license. Python standard library only.
"""
import argparse
from pathlib import Path
import struct
import wave
import zlib


def polygon(x, y, vertices):
    inside = False
    previous = vertices[-1]
    for point in vertices:
        ax, ay = previous
        bx, by = point
        if (ay > y) != (by > y) and x < (bx - ax) * (y - ay) / (by - ay) + ax:
            inside = not inside
        previous = point
    return inside


def pixel(kind, x, y):
    clear, dark, light, accent = (0, 0, 0, 0), (20, 29, 43, 255), (242, 247, 255, 255), (65, 208, 205, 255)
    if kind == 'pointer':
        outer = [(16, 8), (16, 49), (27, 40), (35, 56), (43, 52), (35, 36), (50, 34)]
        inner = [(19, 15), (19, 43), (28, 35), (37, 52), (39, 51), (30, 33), (42, 32)]
        return light if polygon(x, y, inner) else dark if polygon(x, y, outer) else clear
    if kind == 'crosshair':
        radius = (x - 32) ** 2 + (y - 32) ** 2
        if 15 ** 2 <= radius <= 19 ** 2:
            return dark if radius < 16 ** 2 or radius > 18 ** 2 else accent
        if (29 <= x <= 35 and (7 <= y <= 23 or 41 <= y <= 57)) or (29 <= y <= 35 and (7 <= x <= 23 or 41 <= x <= 57)):
            return dark if x in (29, 35) or y in (29, 35) else light
        return accent if radius <= 3 ** 2 else clear
    # Neutral hidden-achievement badge: a geometric frame and a keyhole.
    outer = [(32, 4), (56, 18), (56, 46), (32, 60), (8, 46), (8, 18)]
    inner = [(32, 8), (52, 20), (52, 44), (32, 56), (12, 44), (12, 20)]
    if not polygon(x, y, outer):
        return clear
    if not polygon(x, y, inner):
        return accent
    if (x - 32) ** 2 + (y - 27) ** 2 <= 7 ** 2 or (29 <= x <= 35 and 30 <= y <= 43):
        return light
    return dark


def png(kind):
    raw = b''.join(b'\0' + bytes(c for x in range(64) for c in pixel(kind, x, y)) for y in range(64))
    def chunk(name, content):
        return struct.pack('>I', len(content)) + name + content + struct.pack('>I', zlib.crc32(name + content))
    # Level zero gives deterministic stored DEFLATE blocks, independent of
    # compressor heuristics. All three images are 64x64 RGBA.
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 64, 64, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 0)) + chunk(b'IEND', b''))


def generate(root):
    for path, kind in [('cursors/hand.png', 'pointer'), ('cursors/drawing.png', 'crosshair'),
                       ('images/hidden-trophy.png', 'badge')]:
        dest = root / path
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(png(kind))
    dest = root / 'sounds/trophy-unlock.wav'
    dest.parent.mkdir(parents=True, exist_ok=True)
    rate, frames = 48000, 16800
    samples = []
    for i in range(frames):
        frequency = 660 if i < 7200 else 880
        local = i if i < 7200 else i - 7200
        duration = 7200 if i < 7200 else frames - 7200
        envelope = min(local, 480, duration - 1 - local)
        triangle = 4 * abs((local * frequency) % rate - rate // 2) - rate
        samples.append(triangle * max(0, envelope) * 4500 // (rate * 480))
    with wave.open(str(dest), 'wb') as output:
        output.setparams((1, 2, rate, frames, 'NONE', 'not compressed'))
        output.writeframes(struct.pack('<' + 'h' * frames, *samples))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='Asset directory to generate/replace')
    generate(parser.parse_args().output)
