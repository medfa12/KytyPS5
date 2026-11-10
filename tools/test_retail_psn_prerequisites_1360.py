"""Synthetic offline controls; no PSN credentials, stores or network access."""
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import padding, rsa

from audit_retail_psn_prerequisites_1360 import inspect_core, inspect_frozen_log, inspect_np_manager, read_virtual, verify_envelope


class RetailPsnPrerequisiteTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Test-origin key/envelope only: this is not a Sony credential/token.
        private = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        cls.key = private.public_key()
        frame = bytearray(0x1e0)
        frame[:4] = bytes.fromhex('cccb7351')
        struct.pack_into('<II', frame, 4, 1, 0x28)
        struct.pack_into('<Q', frame, 0x10, 16)
        frame[0x1d0:] = b'test-only-frame!'
        frame[0x50:0x1d0] = private.sign(
            bytes(frame[:0x50]) + bytes(0x180) + bytes(frame[0x1d0:]),
            padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=padding.PSS.MAX_LENGTH),
            hashes.SHA256())
        cls.frame = bytes(frame)

    def test_signature_and_corruption_controls(self):
        self.assertEqual(verify_envelope(self.frame, self.key)['payload_bytes'], 16)
        for offset in (0x50, 0x1df):
            corrupt = bytearray(self.frame)
            corrupt[offset] ^= 1
            with self.assertRaises(InvalidSignature):
                verify_envelope(bytes(corrupt), self.key)

    def test_malformed_header_and_length_controls(self):
        for offset in (0, 4, 8, 0x10):
            corrupt = bytearray(self.frame)
            corrupt[offset] ^= 1
            with self.assertRaises(ValueError):
                verify_envelope(bytes(corrupt), self.key)
        for size in (0, 0x50, 0x1d0, 0x1df):
            with self.assertRaises(ValueError):
                verify_envelope(self.frame[:size], self.key)

    def test_mapping_bounds_and_ambiguity(self):
        segment = {'p_vaddr': 0x1000, 'p_filesz': 4, 'p_offset': 2}
        self.assertEqual(read_virtual(b'xxabcd', [segment], 0x1001, 2), b'bc')
        for address, size in ((0xfff, 1), (0x1003, 2), (0x1004, 1), (-1, 1), (0x1000, -1)):
            with self.assertRaises(ValueError):
                read_virtual(b'xxabcd', [segment], address, size)
        with self.assertRaises(ValueError):
            read_virtual(b'xxabcd', [segment, segment], 0x1000, 1)
        with self.assertRaises(ValueError):
            read_virtual(b'xx', [segment], 0x1000, 1)

    def test_wrong_core_rejected_without_mutation(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'not-firmware.elf'
            source.write_bytes(b'test-origin non-firmware')
            before = source.read_bytes()
            with self.assertRaises(ValueError):
                inspect_core(source)
            with self.assertRaises(ValueError):
                inspect_np_manager(source)
            self.assertEqual(source.read_bytes(), before)

    def test_reducer_does_not_echo_sensitive_or_injected_text(self):
        # Deliberately fake markers: fixtures must not emit arbitrary log text.
        sensitive = 'TEST_ONLY_DO_NOT_ECHO_THIS_VALUE'
        content = ('Authorization: Bearer ' + sensitive + '\n' +
                   'https://example.invalid/?code=' + sensitive + '\n' +
                   'NP manager client: RegisterEvent7 service=SceNpMgrIpc result=0x80550002 owner_endpoint_unavailable\n' +
                   'ps5-vsh: open ENOENT: /dev/envelope (real: ' + sensitive + ')\n' +
                   'NP local-account HLE: pid=100 user=0x10010029 state=1 (local profile, no online session)\n' +
                   '[SceShellCore] PsnCore OK, 0 sec\n' +
                   sensitive + ' NP local-account HLE: pid=100 user=0x10010029 state=2 (local profile, no online session)\n')
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'synthetic-guest.log'
            source.write_bytes(content.encode())
            result = inspect_frozen_log(source)
            text = json.dumps(result)
            self.assertNotIn(sensitive, text)
            self.assertNotIn('10010029', text)
            self.assertFalse(result['psn_authentication_verified'])
            self.assertEqual(result['source_sha256'], hashlib.sha256(content.encode()).hexdigest())
            observations = result['observations']
            self.assertEqual(observations['envelope_open_missing']['first_line'], 4)
            self.assertEqual(observations['local_profile_not_signed_up']['count'], 1)
            self.assertEqual(observations['local_profile_signed_out']['count'], 0)
            self.assertEqual(observations['np_owner_endpoint_unavailable']['count'], 1)
            self.assertEqual(source.read_bytes(), content.encode())


if __name__ == '__main__':
    unittest.main()
