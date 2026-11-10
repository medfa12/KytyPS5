"""Offline verifier tests. Set SHELL_ALT_FIRMWARE for retained-input crypto checks."""
import os
import struct
from pathlib import Path
import unittest

from cryptography.exceptions import InvalidSignature
from verify_alternate_shell_credentials_1360 import (
    authenticate, decode_record, load_context, pinned, verify_target,
    verify_segments,
)


class DecoderTests(unittest.TestCase):
    def test_endian_mapping_and_absent_defaults(self):
        payload = bytes(range(48))
        info = bytes(range(64, 128))
        record = decode_record(payload, info)
        self.assertEqual(len(record), 0x88)
        self.assertEqual(record[:8], info[:8])
        self.assertEqual(record[8:24], payload[:8][::-1] + payload[8:16][::-1])
        self.assertEqual(record[0x18:0x28], bytes(16))
        self.assertEqual(record[0x28:0x38], payload[32:40][::-1] + payload[40:48][::-1])
        self.assertEqual(record[0x38:0x48], payload[16:24][::-1] + payload[24:32][::-1])
        self.assertEqual(record[0x48:], bytes(64))

    def test_optional_record_and_invalid_requested_rank(self):
        payload = bytearray(0x50)
        payload[0x30] = 1
        payload[0x40:] = bytes(range(16))
        record = decode_record(payload, bytes(64))
        self.assertEqual(record[0x48:0x58], bytes(range(8))[::-1] + bytes(range(8, 16))[::-1])
        payload[0x31] = 1  # index8 present but rank1 exceeds capacity1
        with self.assertRaises(ValueError):
            decode_record(payload, bytes(64))

    def test_unrequested_trailing_bitmap_bit_not_global_error(self):
        payload = bytearray(0x50)
        payload[0x3f] = 0x80
        self.assertEqual(decode_record(payload, bytes(64)), bytes(0x88))

    def test_source_pin_mismatch(self):
        with self.assertRaises(ValueError):
            pinned(b'changed ELF', '0' * 64, 'ELF')


@unittest.skipUnless(os.environ.get('SHELL_ALT_FIRMWARE'), 'Retained firmware not configured')
class RetainedInputTests(unittest.TestCase):
    def test_both_signatures_and_independent_tamper_rejection(self):
        tree = Path(os.environ['SHELL_ALT_FIRMWARE'])
        context = load_context(tree)
        for target in ('core', 'ui'):
            report, record, container, plain, elf = verify_target(tree, target, context)
            self.assertFalse(report['runtime_credential_eligible'])
            self.assertTrue(report['all_pt_load_bytes_verified'])
            self.assertEqual(int.from_bytes(record[8:16], 'little'), 0x40001c0000000000)
            for offset in (0x4f0, 0x9b0, 0xa20):
                with self.subTest(target=target, offset=offset):
                    changed = bytearray(container)
                    changed[offset] ^= 1
                    with self.assertRaises(InvalidSignature):
                        authenticate(changed, context)
            with self.assertRaises(ValueError):
                pinned(elf[:-1] + bytes([elf[-1] ^ 1]), report['elf_sha256'], 'ELF')
            changed = bytearray(container)
            changed[struct.unpack_from('<Q', container, 40)[0]] ^= 1
            with self.assertRaisesRegex(ValueError, 'HMAC'):
                verify_segments(changed, plain, elf, 12)
            changed = bytearray(elf)
            changed[0x4000] ^= 1
            with self.assertRaisesRegex(ValueError, 'content mismatch'):
                verify_segments(container, plain, changed, 12)


if __name__ == '__main__':
    unittest.main()
