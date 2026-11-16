"""Offline, source-pinned SELF metadata verification; never installs credentials.

Outputs contain hashes and, only when requested, a local decoded auth record.
    Segment authentication additionally binds every extracted ELF byte through a
    checked reconstruction manifest; native secure-service policy is separate.
"""
import argparse
import contextlib
import hashlib
import hmac
import io
import json
from pathlib import Path
import struct
import zlib

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

from inspect_shell_auth_images_1360 import Image

SM_HASH = '990711f28f502255695fe61b5e60364ad29ea525faf30ca5aa5709520af3039f'
TARGETS = {
    'core': ('system_b', 'vsh/SceShellCore.elf',
             '0e365cf5850f156cc099777faf12aa12ba4fb2e71d7d49d82f2aaf5b7708beab',
             'f2e569cebe324129a8be6a40a429e6f6652021f261ded3c02a5469cc81f5a434'),
    'ui': ('system_ex_b', 'app/NPXS40087/eboot.bin',
           '73fd5ed27d0341620f7431598f2828f7206665b39675739dec942740bd703130',
           'f3068cd9769711f9962367026f0bbba28ac2fac24e34fad1f40714979c89e060'),
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def extent(data, offset, length):
    require(0 <= offset <= len(data) and 0 <= length <= len(data) - offset,
            'Out-of-bounds extent')
    return data[offset:offset + length]


def pinned(data, expected, label):
    require(sha(data) == expected, label + ' source hash mismatch')
    return data


def decode_record(payload, extended):
    """SM 0xd94/0xe82/0x332c: zero defaults, individual optional lookups.

    Only the first returned record of each lookup is consumed by construction.
    Subsequent-contiguous lookup behavior cannot alter that first result.
    This is construction, NOT mailbox 0x16 policy/service validation.
    """
    require(len(payload) >= 0x30 and len(extended) == 64, 'Invalid auth extent')
    record = bytearray(0x88)
    record[:8] = extended[:8]
    for dst, src in ((8, 0), (16, 8), (0x28, 0x20), (0x30, 0x28),
                     (0x38, 0x10), (0x40, 0x18)):
        record[dst:dst + 8] = payload[src:src + 8][::-1]
    for index, dst in [(i, 0x48 + 16 * i) for i in range(4)] + [(8, 0x18)]:
        if len(payload) < 0x50:
            continue  # native -2: optional area absent
        bitmap = int.from_bytes(payload[0x30:0x40], 'little')
        if not (bitmap >> index) & 1:
            continue  # native -2: requested bit absent
        rank = (bitmap & ((1 << index) - 1)).bit_count()
        capacity = ((len(payload) - 0x30) >> 4) - 1
        require(rank < capacity, 'Optional record rank out of bounds (native -34)')
        src = 0x40 + rank * 16
        record[dst:dst + 8] = payload[src:src + 8][::-1]
        record[dst + 8:dst + 16] = payload[src + 8:src + 16][::-1]
    return bytes(record)


def load_context(tree):
    sm = pinned((tree / 'decrypted_modules/sm_authmgr.elf').read_bytes(), SM_HASH, 'SM')
    phoff = struct.unpack_from('<I', sm, 28)[0]
    stride, count = struct.unpack_from('<HH', sm, 42)
    require(stride == 32, 'Unsupported SM program headers')

    def mapped(va, size):
        for i in range(count):
            typ, off, base, _, length, *_ = struct.unpack('<8I', extent(sm, phoff + i * stride, stride))
            if typ == 1 and base <= va and va + size <= base + length:
                return extent(sm, off + va - base, size)
        raise ValueError('SM virtual address unmapped')

    dictionary = json.loads((tree / 'key_dictionary.json').read_text())
    keys = [bytes.fromhex(k['key']) for k in dictionary['ccp_keys'] if k['id'] == 41]
    require(len(keys) == 1 and len(keys[0]) == 16, 'Expected one AES-128 slot41 candidate')
    mp, ml, ep, el = struct.unpack('<4I', mapped(0x8024, 16))
    require(ml == 0x180, 'Unsupported RSA modulus length')
    pub = rsa.RSAPublicNumbers(int.from_bytes(mapped(ep, el), 'big'),
                              int.from_bytes(mapped(mp, ml), 'big')).public_key()
    return keys[0], mapped(0x62d0, 16), pub


def authenticate(container, context):
    key, iv, pub = context
    require(len(container) >= 32 and container[:4] == bytes.fromhex('5414f5ee'), 'Not SELF')
    hs, ms = struct.unpack_from('<HH', container, 12)
    n = struct.unpack_from('<H', container, 24)[0] & 0xfff
    require(container[7] == 0x32 and (container[26] >> 4) & 7 == 5,
            'Unsupported transform/layout')
    require(ms % 16 == 0 and ms >= n * 0x60 + 0x30 + 0x180, 'Invalid metadata size')
    decryptor = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
    plain = decryptor.update(extent(container, hs, ms)) + decryptor.finalize()
    pub.verify(plain[-0x180:], extent(container, 0, hs) + plain[:-0x180],
               padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32), hashes.SHA256())
    return plain, n


def decrypt_segment(data, key, iv):
    """SM 0x3984/0x403c: CBC full blocks; ECB(last IV) XOR partial tail."""
    aligned = len(data) & ~15
    decryptor = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
    result = decryptor.update(data[:aligned]) + decryptor.finalize()
    if aligned != len(data):
        encryptor = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
        mask = encryptor.update(data[aligned - 16:aligned] if aligned else iv) + encryptor.finalize()
        result += bytes(a ^ b for a, b in zip(data[aligned:], mask))
    return result


def verify_segments(container, plain, elf, n):
    """Authenticate ciphertext, decrypt each block, and compare every ELF byte.

    Deliberately limited to the pinned pair's six table/data entry pairs.
    Original whole-ELF padding and appended notes are NOT signed code evidence.
    """
    require(n == 12, 'Unsupported SELF entry count')
    entries = [struct.unpack_from('<4Q', container, 32 + i * 32) for i in range(n)]
    metadata = [extent(plain, i * 96, 96) for i in range(n)]
    for i, (_, off, size, _) in enumerate(entries):
        require(hmac.compare_digest(hmac.digest(metadata[i][64:96], extent(container, off, size),
                                                'sha3_256'), metadata[i][32:64]),
                f'SELF entry {i} HMAC-SHA3 mismatch')
    reports = []
    for i in range(0, n, 2):
        flags, off, size, decoded_size = entries[i]
        sf, so, ss, sd = entries[i + 1]
        require(flags == ((i + 1) << 20) | (0x30006 if i < 10 else 0x10006),
                'Unsupported digest-table flags')
        require(sf & 0xfffff == (0x2c0e if i < 10 else 0x2c06), 'Unsupported data flags')
        require(size == decoded_size, 'Compressed table unsupported')
        m, sm = metadata[i], metadata[i + 1]
        table = decrypt_segment(extent(container, off, size), m[:16], m[16:32])
        block_size = 0x1000 << ((sf >> 12) & 15)
        count = (sd + block_size - 1) // block_size
        require(len(table) == count * (40 if flags & 0x20000 else 32), 'Invalid digest table size')
        phindex = sf >> 20
        require(phindex < struct.unpack_from('<H', elf, 56)[0], 'Invalid program header index')
        ph = struct.unpack('<II6Q', extent(elf, 64 + phindex * 56, 56))
        require(sd == ph[5], 'Decoded size differs from program header')
        blocks = []
        for j in range(count):
            remaining = min(block_size, sd - j * block_size)
            if flags & 0x20000:
                offset, encoded_length = struct.unpack_from('<II', table, count * 32 + j * 8)
                length = encoded_length if j == count - 1 and encoded_length == remaining else encoded_length & ~15
            else:
                offset, length = j * block_size, remaining
            require(offset + length <= ss, 'Block exceeds entry')
            raw = extent(container, so + offset, length)
            require(hmac.compare_digest(hmac.digest(sm[64:96], raw, 'sha3_256'), table[j * 32:j * 32 + 32]),
                    f'SELF entry {i + 1} block {j} HMAC mismatch')
            decoded = decrypt_segment(raw, sm[:16], sm[16:32])
            if sf & 8 and length != remaining:
                inflater = zlib.decompressobj()
                decoded = inflater.decompress(decoded, remaining + 1)
                require(inflater.eof and not inflater.unconsumed_tail, 'Invalid compressed block')
            require(len(decoded) == remaining, 'Invalid decoded block size')
            require(decoded == extent(elf, ph[2] + j * block_size, remaining),
                    f'ELF segment {phindex} block {j} content mismatch')
            blocks.append(decoded)
        reports.append(dict(program_header=phindex, offset=ph[2], size=sd,
                            blocks=count, plaintext_sha256=sha(b''.join(blocks))))
    for p in range(struct.unpack_from('<H', elf, 56)[0]):
        ph = struct.unpack_from('<II6Q', elf, 64 + p * 56)
        if ph[0] == 1 and ph[5]:
            require(any(r['offset'] <= ph[2] and ph[2] + ph[5] <= r['offset'] + r['size'] for r in reports),
                    'Unverified PT_LOAD bytes')
    return reports


def verify_target(tree, target, context):
    volume, member, self_hash, elf_hash = TARGETS[target]
    image = Image(tree / 'segments' / ('dev_ssd0.' + volume))
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            container = image.target(member.split('/'))
    finally:
        image.file.close()
    pinned(container, self_hash, 'SELF')
    elf = pinned((tree / volume / member).read_bytes(), elf_hash, 'ELF')
    plain, n = authenticate(container, context)
    e = 32 + n * 32
    require(extent(container, e, 64) == extent(elf, 0, 64), 'ELF header mismatch')
    phoff = struct.unpack_from('<Q', elf, 32)[0]
    stride, count = struct.unpack_from('<HH', elf, 54)
    require(stride == 56 and phoff == 64, 'Unsupported ELF layout')
    require(extent(container, e + phoff, count * stride) == extent(elf, phoff, count * stride),
            'Program header mismatch')
    info = extent(container, (e + 0x4f + count * 56) & ~15, 64)
    require(info == extent(elf, phoff + count * stride, 64), 'Extended-info mismatch')
    record = decode_record(plain[n * 0x60:-0x180], info)
    segments = verify_segments(container, plain, elf, n)
    # Validate the extraction transformation, including otherwise unsigned gaps:
    # zero-initialize to the highest represented segment end, copy signed header,
    # PHDRs and extended info, then the six verified plaintext segment extents.
    reconstructed = bytearray(max(r['offset'] + r['size'] for r in segments))
    header_end = phoff + count * stride
    reconstructed[:header_end] = extent(container, e, header_end)
    reconstructed[header_end:header_end + 64] = info
    for segment in segments:
        start, size = segment['offset'], segment['size']
        reconstructed[start:start + size] = extent(elf, start, size)
    require(bytes(reconstructed) == elf, 'ELF extraction manifest mismatch (including padding/trailer)')
    report = dict(target=target, self_sha256=self_hash, elf_sha256=elf_hash,
                  sm_sha256=SM_HASH, metadata_signature_verified=True,
                  structural_pairing_verified=True, full_code_provenance_verified=True,
                  runtime_credential_eligible=False, auth_record_sha256=sha(record),
                  auth_record_size=len(record), policy_validation='not executed',
                  all_self_segments_verified=True, all_pt_load_bytes_verified=True,
                  segments=segments, transformation=dict(
                      name='zero-fill-signed-headers-and-authenticated-segments-v1',
                      output_size=len(elf), output_sha256=sha(reconstructed),
                      header_size=header_end, extended_info_size=64,
                      note='Preserves signed PHDRs, including any advertised note beyond EOF; no guest patches'))
    return report, record, container, plain, elf


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New user-local JSON; refuses overwrite')
    parser.add_argument('--include-record', action='store_true', help='Include decoded metadata record; NOT runtime eligible')
    parser.add_argument('--core-record-output', type=Path, help='New local raw 136-byte Core record for the pinned host provider')
    args = parser.parse_args()
    context = load_context(args.firmware)
    reports = []
    core_record = None
    for target in TARGETS:
        report, record, *_ = verify_target(args.firmware, target, context)
        if target == 'core':
            core_record = record
        if args.include_record:
            report['metadata_decoded_record_hex'] = record.hex()
        reports.append(report)
    with args.output.open('x', encoding='utf-8') as output:
        json.dump(dict(schema='shell-metadata-evidence-v1', targets=reports), output, indent=2)
        output.write('\n')
    if args.core_record_output:
        with args.core_record_output.open('xb') as output:
            output.write(core_record)
    print('Verified both signatures, all SELF/PT_LOAD segments and exact ELF reconstruction; native policy remains unexecuted.')


if __name__ == '__main__':
    main()
