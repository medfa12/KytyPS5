"""Preview captured CPU texture backing; this is not synchronized GPU readback.

Supports the base-level, single-layer R8, RGBA8 and BC7 standard-tiled textures
observed in ShellUI. Requires Pillow. Unsupported descriptors are skipped.
"""
import argparse
import json
from pathlib import Path

from PIL import Image, ImageDraw


def standard_offset(x, y, element_bytes):
    if element_bytes == 1:
        return (((y << 4) & 0x1f0) ^ ((y << 5) & 0x400) ^ (x & 0xf)
                ^ ((x << 5) & 0x200) ^ ((x << 6) & 0x800))
    if element_bytes == 4:
        return (((y << 4) & 0x70) ^ ((y << 5) & 0x100) ^ ((y << 6) & 0x400)
                ^ ((x << 2) & 0xc) ^ ((x << 5) & 0x80)
                ^ ((x << 6) & 0x200) ^ ((x << 7) & 0x800))
    return (((y << 4) & 0x30) ^ ((y << 6) & 0x100) ^ ((y << 7) & 0x400)
            ^ ((x << 6) & 0xc0) ^ ((x << 7) & 0x200) ^ ((x << 8) & 0x800))


def preview(entry, directory):
    words = [int(word, 0) for word in entry['descriptor']]
    if (entry['format'] not in (1, 56, 181) or entry['tile'] not in (1, 5)
            or (words[3] >> 28) != 9 or (words[3] & 0xff000)
            or (words[4] & 0x1fff) or 'file' not in entry):
        return None
    w, h = entry['width'], entry['height']
    if not 0 < w * h <= 4 * 1024 * 1024:
        return None
    filename = directory / entry['file']
    if filename.resolve().parent != directory.resolve():
        raise ValueError('Texture file is outside the capture directory')
    raw = filename.read_bytes()
    if len(raw) != entry['size'] or len(raw) > 4 * 1024 * 1024:
        raise ValueError('Texture backing size mismatch')
    bc7 = entry['format'] == 181
    b = 16 if bc7 else (4 if entry['format'] == 56 else 1)
    ew, eh = ((w + 3) // 4, (h + 3) // 4) if bc7 else (w, h)
    block = 256 if entry['tile'] == 1 else 4096
    bw = ({1: 16, 4: 8, 16: 4} if block == 256 else {1: 64, 4: 32, 16: 16})[b]
    bh = block // (bw * b)
    columns = (ew + bw - 1) // bw
    if columns * ((eh + bh - 1) // bh) * block > len(raw):
        raise ValueError('Tiled extent exceeds captured backing')
    linear = bytearray(ew * eh * b)
    for y in range(eh):
        for x in range(ew):
            source = (((y // bh) * columns + x // bw) * block
                      + (standard_offset(x % bw, y % bh, b) & (block - 1)))
            dest = (y * ew + x) * b
            linear[dest:dest + b] = raw[source:source + b]
    if bc7:
        return Image.frombytes('RGBA', (ew * 4, eh * 4), bytes(linear), 'bcn', 7).crop((0, 0, w, h))
    return Image.frombytes('RGBA' if b == 4 else 'L', (w, h), bytes(linear))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    manifest = args.directory / 'textures.json'
    if manifest.exists():
        entries = json.loads(manifest.read_text(encoding='utf-8'))['textures']
    else:
        entries = [json.loads(line) for line in
                   (args.directory / 'textures.jsonl').read_text(encoding='utf-8').splitlines()]
    if not 0 < len(entries) <= 128:
        parser.error('Expected 1..128 texture bindings')
    sheet = Image.new('RGB', (800, ((len(entries) + 3) // 4) * 180), '#353535')
    draw = ImageDraw.Draw(sheet)
    count = 0
    for index, entry in enumerate(entries):
        x, y = (index % 4) * 200, (index // 4) * 180
        draw.text((x + 3, y + 5), f"{index}: {entry['width']}x{entry['height']} CPU", fill='white')
        image = preview(entry, args.directory)
        if image is None:
            draw.text((x + 3, y + 30), 'Unsupported / unavailable', fill='white')
            continue
        image.save(args.directory / (entry['file'] + '.png'))
        image.thumbnail((190, 150))
        sheet.paste(image, (x + 3, y + 25), image if image.mode == 'RGBA' else None)
        count += 1
    sheet.save(args.directory / 'contact.png')
    print(f'{count} CPU backing previews: {args.directory / "contact.png"}')


if __name__ == '__main__':
    main()
