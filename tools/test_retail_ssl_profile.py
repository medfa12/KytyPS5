"""Read-only retail SSL admission audit against actual ELF metadata.

No firmware execution or generated firmware files. Requires pyelftools.
The optional old header supplies a failing-old profile control.
"""
import argparse
import hashlib
import io
from pathlib import Path
import re
import struct

from elftools.elf.elffile import ELFFile

SSL = 'c4cd758bb1f9aa25ffef517e89e029ef4b3f6f3b22b7e525c1e4bf591b8fb3a7'
LIBC = '8fa838791061e19ca81ddd4912798bfd1d074f8127ce8c5fb25bb708eab66935'
CORE = 'f2e569cebe324129a8be6a40a429e6f6652021f261ded3c02a5469cc81f5a434'


class Image:
    def __init__(self, path, digest):
        self.data = Path(path).read_bytes()
        if hashlib.sha256(self.data).hexdigest() != digest:
            raise ValueError('wrong source image hash')
        elf = ELFFile(io.BytesIO(self.data))
        self.loads = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
        dynamic = next(s for s in elf.iter_segments() if s['p_type'] == 'PT_DYNAMIC')
        self.dynamic = list(struct.iter_unpack('<QQ', dynamic.data()))
        self.tags = dict(self.dynamic)
        self.symbols = []
        for offset in range(0, self.tags[0x6100003f], 24):
            name, info, _, section, address, size = struct.unpack('<IBBHQQ', self.read(self.tags[6] + offset, 24))
            if name:
                self.symbols.append((self.string(name), info, section, address, size))

    def read(self, address, size):
        for s in self.loads:
            if s['p_vaddr'] <= address and address + size <= s['p_vaddr'] + s['p_filesz']:
                off = s['p_offset'] + address - s['p_vaddr']
                return self.data[off:off + size]
        raise ValueError('unmapped file range')

    def string(self, offset):
        out = bytearray()
        for n in range(1024):
            byte = self.read(self.tags[5] + offset + n, 1)
            if byte == b'\0':
                return out.decode()
            out.extend(byte)
        raise ValueError('unterminated string')


def profile(text):
    body = text.split('SslPublicExports[] {', 1)[1].split('};', 1)[0]
    return {n: (int(a, 16), int(s, 16)) for n, a, s in
            re.findall(r'\{"([^"]+)",\s*(0x[\da-f]+),\s*(0x[\da-f]+)\}', body)}


def check_exports(image, expected):
    actual = {}
    for raw, info, section, address, size in image.symbols:
        if section and raw.endswith('#G#A'):
            if info != 0x12 or raw[:-4] in actual:
                raise ValueError('wrong binding/type or duplicate')
            actual[raw[:-4]] = (address, size)
    if len(expected) != 57 or actual != expected:
        raise ValueError('native export profile mismatch')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ssl', required=True)
    p.add_argument('--libc', required=True)
    p.add_argument('--core', required=True)
    p.add_argument('--old-header', type=Path)
    args = p.parse_args()
    text = (Path(__file__).resolve().parents[1] / 'src/loader/nativeSslSmoke.h').read_text()
    pin = text.split('SslHash[32] {', 1)[1].split('};', 1)[0]
    assert bytes(int(v, 16) for v in re.findall(r'0x([\da-f]{2})', pin)).hex() == SSL
    ssl, libc, core = Image(args.ssl, SSL), Image(args.libc, LIBC), Image(args.core, CORE)
    exports = profile(text)
    check_exports(ssl, exports)
    assert ssl.tags[12] == 0x20 and libc.tags[12] == 0x10
    # Same retained runtime-state slots, including zero-fill tails.
    for address in [0x169178, 0x169188, 0x16a060, 0x16a458]:
        assert any(s['p_flags'] & 6 == 6 and s['p_vaddr'] <= address and
                   address + 8 <= s['p_vaddr'] + s['p_memsz'] for s in ssl.loads)
    imports = [raw for raw, _, section, _, _ in core.symbols if not section and raw.split('#')[0] in exports]
    assert len(imports) == 21 and all(raw.endswith('#Q#Q') for raw in imports)
    # Decode actual Sony library/module identities, not the HLE log aliases.
    for image, library_tags, module_tags, library_id, module_id in [
        (ssl, (0x61000013, 0x61000047), (0x6100000d, 0x61000043), 6, 0),
        (core, (0x61000015, 0x61000049), (0x6100000f, 0x61000045), 16, 16),
    ]:
        libraries = [v for t, v in image.dynamic if t in library_tags and v >> 48 == library_id]
        modules = [v for t, v in image.dynamic if t in module_tags and v >> 48 == module_id]
        assert len(libraries) == len(modules) == 1
        assert image.string(libraries[0] & 0xffffffff) == 'libSceSsl'
        assert (libraries[0] >> 32) & 0xffff == 1
        assert image.string(modules[0] & 0xffffffff) == 'libSceSsl'
        assert (modules[0] >> 32) & 0xffff == 0x201
    # Verify required native libc bindings against the actual SSL imports.
    body = text.split('LibcExports[] {', 1)[1].split('};', 1)[0]
    required = set(re.findall(r'\{"([^"]+)"', body))
    native_imports = {raw.split('#')[0] for raw, _, section, _, _ in ssl.symbols if not section}
    assert len(required) == 15 and required <= native_imports
    for bad in [{}, {**exports, 'TDfQqO-gMbY': (0x22a0, 0x1f6)},
                {**exports, 'w1+L-27nYas': (0x2a10, 0x12)}]:
        try:
            check_exports(ssl, bad)
        except ValueError:
            pass
        else:
            raise AssertionError('negative export control admitted')
    if args.old_header:
        try:
            check_exports(ssl, profile(args.old_header.read_text()))
        except ValueError:
            print('exact old profile rejected against actual retail SSL (expected)')
        else:
            raise AssertionError('old profile unexpectedly admitted')
    print('retail SSL: pinned sources, 57 exact exports, 21 Core imports, 15 libc dependencies, init/state ranges and negative controls passed')


if __name__ == '__main__':
    main()
