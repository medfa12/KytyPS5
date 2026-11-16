"""Read-only targeted exFAT traversal, including truncated-volume evidence.

No mounting, extraction or writes. Checks directory-set checksums and physical
extent bounds; does not authenticate SELF metadata. Reads only named targets.
"""
from pathlib import Path
import hashlib
import struct


class Image:
    def __init__(self, path):
        self.path = path
        self.file = path.open('rb')
        self.size = path.stat().st_size
        header = self.read(0, 512)
        if header[3:11] != b'EXFAT   ' or header[510:512] != b'\x55\xaa':
            raise ValueError('Not an exFAT boot sector')
        sector, cluster = header[0x6c:0x6e]
        if not 9 <= sector <= 12 or sector + cluster > 25:
            raise ValueError('Unsupported sector/cluster extent')
        self.sector = 1 << sector
        self.cluster = 1 << (sector + cluster)
        self.fat, self.fat_length, self.heap, self.count, self.root = struct.unpack_from('<5I', header, 0x50)
        self.declared = struct.unpack_from('<Q', header, 0x48)[0] * self.sector

    def read(self, offset, size):
        if offset < 0 or size < 0 or offset + size > self.size:
            raise ValueError(f'Physical extent unavailable: {offset:#x}+{size:#x}, image={self.size:#x}')
        self.file.seek(offset)
        result = self.file.read(size)
        if len(result) != size:
            raise ValueError('Short image read')
        return result

    def clusters(self, first, contiguous=False, length=None):
        seen = set()
        while first < 0xfffffff8:
            if not 2 <= first < self.count + 2 or first in seen:
                raise ValueError('Invalid/cyclic cluster chain')
            seen.add(first)
            if len(seen) > 8192:
                raise ValueError('Target exceeds bounded traversal')
            size = self.cluster if length is None else min(self.cluster, length)
            yield self.read(self.heap * self.sector + (first - 2) * self.cluster, size)
            if length is not None:
                length -= size
                if length == 0:
                    return
            if contiguous:
                first += 1
            else:
                if first * 4 + 4 > self.fat_length * self.sector:
                    raise ValueError('FAT index outside declared table')
                first = struct.unpack('<I', self.read(self.fat * self.sector + first * 4, 4))[0]
        if length:
            raise ValueError('Chain shorter than declared target')

    def directory(self, entry=None):
        if entry is None:
            chunks = self.clusters(self.root)
        else:
            chunks = self.clusters(entry['cluster'], entry['contiguous'], entry['length'])
        raw = b''.join(chunks)
        offset = 0
        while offset + 32 <= len(raw) and raw[offset]:
            kind = raw[offset]
            if kind != 0x85:
                offset += 32
                continue
            size = (raw[offset + 1] + 1) * 32
            group = raw[offset:offset + size]
            if len(group) != size or size < 96:
                raise ValueError('Truncated directory entry set')
            checksum = 0
            for index, byte in enumerate(group):
                if index not in (2, 3):
                    checksum = (((checksum >> 1) | (checksum << 15)) + byte) & 0xffff
            if checksum != struct.unpack_from('<H', group, 2)[0]:
                raise ValueError('Directory entry checksum mismatch')
            if group[32] != 0xc0:
                raise ValueError('Missing stream extension')
            name_bytes = b''.join(group[i + 2:i + 32] for i in range(64, size, 32) if group[i] == 0xc1)
            name = name_bytes[:group[35] * 2].decode('utf-16le')
            yield dict(name=name, directory=bool(group[4] & 0x10), contiguous=bool(group[33] & 2),
                       cluster=struct.unpack_from('<I', group, 52)[0],
                       valid=struct.unpack_from('<Q', group, 40)[0],
                       length=struct.unpack_from('<Q', group, 56)[0])
            offset += size

    def target(self, parts):
        entry = None
        for index, part in enumerate(parts):
            entries = list(self.directory(entry))
            entry = next((e for e in entries if e['name'].casefold() == part.casefold()), None)
            if entry is None:
                raise ValueError(f'Missing {part!r}; available names: {[e["name"] for e in entries]}')
            if index < len(parts) - 1 and not entry['directory']:
                raise ValueError('Intermediate target is not a directory')
        if entry['directory'] or not 0 < entry['length'] <= 128 * 1024 * 1024 or entry['valid'] != entry['length']:
            raise ValueError(f'Unexpected executable stream: {entry}')
        data = b''.join(self.clusters(entry['cluster'], entry['contiguous'], entry['length']))
        print('TARGET', '/'.join(parts), entry)
        print('SHA256', hashlib.sha256(data).hexdigest(), 'prefix32', data[:32].hex())
        if data[:5] == b'\x7fELF\x02':
            phoff = struct.unpack_from('<Q', data, 32)[0]
            extent, count = struct.unpack_from('<HH', data, 54)
            if extent != 56 or phoff + extent * count > len(data):
                raise ValueError('Invalid ELF program header table')
            info = phoff + extent * count
            loads = [struct.unpack_from('<Q', data, phoff + i * extent + 8)[0]
                     for i in range(count) if struct.unpack_from('<I', data, phoff + i * extent)[0] == 1]
            first = min(loads)
            print('extended-info', hex(info), data[info:info + 64].hex())
            print('gap', hex(info + 64), hex(first), 'nonzero', sum(v != 0 for v in data[info + 64:first]))
        elif data[:4] == bytes.fromhex('5414f5ee'):
            hs, ms = struct.unpack_from('<HH', data, 12)
            n = struct.unpack_from('<H', data, 24)[0] & 0xfff
            elf = 0x20 + n * 0x20
            phnum = struct.unpack_from('<H', data, elf + 0x38)[0]
            info = (elf + 0x4f + phnum * 0x38) & ~15
            sig, layout = (data[7] >> 4) & 7, (data[26] >> 4) & 7
            stride, trailer = {2: 0x50, 3: 0x60, 5: 0x60}[layout], {1: 0x100, 3: 0x180, 4: 0x180}[sig]
            payload, length = hs + n * stride, ms - n * stride - trailer
            print('SELF hs/ms/n/sig/layout', hex(hs), hex(ms), n, sig, layout)
            print('embedded-ELF', hex(elf), 'extended-info', hex(info), data[info:info + 64].hex())
            print('metadata-payload-location', hex(payload), hex(length), '(not decrypted/authenticated)')
        return data


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('firmware_roots', type=Path, nargs='+')
    for tree in parser.parse_args().firmware_roots:
        for volume, target in [('system_b', ['vsh', 'SceShellCore.elf']),
                               ('system_ex_b', ['app', 'NPXS40087', 'eboot.bin'])]:
            path = tree / 'segments' / ('dev_ssd0.' + volume)
            print('\nIMAGE', path)
            image = None
            try:
                image = Image(path)
                print('bytes', image.size, 'declared-volume', image.declared, 'cluster-size', image.cluster)
                data = image.target(target)
                extracted = tree / volume / Path(*target)
                if data[:4] == bytes.fromhex('5414f5ee') and extracted.is_file():
                    decoded = extracted.read_bytes()
                    n = struct.unpack_from('<H', data, 24)[0] & 0xfff
                    elf = 0x20 + n * 0x20
                    phnum = struct.unpack_from('<H', data, elf + 0x38)[0]
                    info = (elf + 0x4f + phnum * 0x38) & ~15
                    decoded_ph = struct.unpack_from('<Q', decoded, 32)[0]
                    extent, count = struct.unpack_from('<HH', decoded, 54)
                    decoded_info = decoded_ph + extent * count
                    print('EXTRACTED', extracted, 'SHA256', hashlib.sha256(decoded).hexdigest())
                    print('extended-info-match', data[info:info + 64] == decoded[decoded_info:decoded_info + 64],
                          'ELF-header-and-PH-match', data[elf:elf + decoded_info] == decoded[:decoded_info])
            except (OSError, ValueError, StopIteration) as error:
                print('UNRESOLVED', error)
            finally:
                if image is not None:
                    image.file.close()
