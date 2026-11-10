"""Firmware-free, stdlib-only validation of the reviewed offline decoder."""
import contextlib
import importlib.util
import io
from pathlib import Path
import struct
import sys
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("socketpair_decoder_under_test", ROOT / "tools" / "decode_socketpair_provenance.py")
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)
U64 = (1 << 64) - 1


def artifact(reservations=(0, 0, 0, 0)):
    data = bytearray(7424)
    header = [decoder.MAGIC, 1, 1234, 7424, 1, 56, 128, 0]
    header += [32, 8, 8, 8] + list(reservations) + [0] * 16
    struct.pack_into("<32Q", data, 0, *header)
    return data


def record(data, index, event, sequence=None, caller=0, fd=8, obj=0x1000,
           generation=1, kind=4, detail=(1, 0, 0x3000, 0, 0, 0), extreme=False):
    seq = index + 1 if sequence is None else sequence
    tid, ctx = (U64, U64) if extreme else (77, 0x2000)
    values = [seq, event, caller, tid, ctx, fd, obj, generation, kind, *detail]
    struct.pack_into("<16Q", data, 256 + index * 128, index + 1, *values)


class DecoderChecks(unittest.TestCase):
    def test_valid_expected_chain(self):
        data = artifact((4, 1, 0, 1))
        record(data, 32, 6, sequence=1, caller=0x9000049DE, fd=U64,
               obj=0x7EF7F2A60, generation=0, kind=7, detail=(1, 1, 0, 0, 8, 8))
        record(data, 0, 3, sequence=2, caller=0x900004A70, generation=10, detail=(0, 0, 0x2000, 0, 0, 0))
        record(data, 1, 4, sequence=3, caller=0x900004A70, generation=10, detail=(0, 0, 0x2000, 0, 0, 0))
        record(data, 2, 1, sequence=4, generation=11, kind=0, detail=(0, 0, 0x4000, 0, 0, 0))
        record(data, 3, 2, sequence=5, generation=11, kind=0, detail=(1, 1, 0x4000, 0, 0, 0))
        record(data, 48, 5, sequence=6, caller=0x900037CCE, generation=11, kind=0, detail=(1, 1, 0x4000, 10, 0, 0))
        text = decoder.decode(data)
        self.assertIn("pre=(8, 0) post=(8, 0) unchanged=True", text)
        self.assertIn("ra=0x0000000900004a70", text)
        self.assertIn("ra=0x0000000900037cce", text)
        self.assertIn("gen=11 kind=file opened=1 gls=1", text)
        self.assertIn("committed=6 incomplete_slots=[]", text)
        self.assertLess(text.index("seq=1 socketpair"), text.index("seq=2 close"))

    def test_full_banks_and_text_bound(self):
        data = artifact(decoder.CAPS)
        for bank, (base, capacity) in enumerate(zip(decoder.OFFSETS, decoder.CAPS)):
            for local in range(capacity):
                index = base + local
                event = (1 + local % 4) if bank == 0 else (6 if bank == 1 else 5)
                kind = 7 if bank == 1 else (4 if bank == 2 else 0)
                detail = (U64, U64, U64, U64, 0x8000000080000000, 0x8000000080000000) if bank == 1 else (1, 1, U64, U64, 0, 0)
                record(data, index, event, sequence=U64-index, caller=U64, fd=U64 >> 1,
                       obj=U64, generation=U64, kind=kind, detail=detail, extreme=True)
        text = decoder.decode(data)
        n = len(text.encode("utf-8"))
        self.assertIn("committed=56 incomplete_slots=[]", text)
        self.assertLessEqual(n, 24000)
        self.assertLess(7424 + n, 32768)

    def test_truncated(self):
        data = artifact()
        for size in (0, 255, 256, 7423):
            with self.subTest(size=size), self.assertRaises(ValueError):
                decoder.decode(data[:size])

    def test_oversized(self):
        for extra in (1, 65536):
            with self.subTest(extra=extra), self.assertRaises(ValueError):
                decoder.decode(artifact() + bytes(extra))

    def test_wrong_header(self):
        for word, value in ((0, 0), (1, 2), (3, 7423), (4, 0), (8, 31), (9, 9), (10, 0), (11, 0)):
            data = artifact()
            struct.pack_into("<Q", data, word * 8, value)
            with self.subTest(word=word), self.assertRaises(ValueError):
                decoder.decode(data)

    def test_uncommitted_slots_ignore_partial_payload(self):
        data = artifact((1, 1, 0, 0))
        for index in (0, 32):
            data[256 + index * 128 + 8:256 + (index + 1) * 128] = b"\xff" * 120
        text = decoder.decode(data)
        self.assertIn("committed=0 incomplete_slots=[0, 32]", text)
        self.assertNotIn("seq=", text)

    def test_invalid_commit(self):
        data = artifact((1, 0, 0, 0))
        record(data, 0, 1)
        struct.pack_into("<Q", data, 256, 2)
        with self.assertRaises(ValueError):
            decoder.decode(data)

    def test_bank_mismatch_and_overreservation(self):
        data = artifact((1, 0, 0, 0))
        record(data, 0, 6)
        with self.assertRaises(ValueError):
            decoder.decode(data)
        with self.assertRaises(ValueError):
            decoder.decode(artifact((33, 0, 0, 0)))

    def test_unreadable_pairs_not_reported_equal(self):
        data = artifact((0, 1, 0, 0))
        record(data, 32, 6, kind=0, detail=(1, 1, 0, 0, 0, 0))
        text = decoder.decode(data)
        self.assertIn("pre=unreadable post=unreadable unchanged=False", text)

    def test_main_input_read_bound_and_valid_output(self):
        class Spy(io.BytesIO):
            def __init__(self, data):
                super().__init__(data)
                self.requests, self.returned = [], []
            def read(self, n=-1):
                self.requests.append(n)
                result = super().read(n)
                self.returned.append(len(result))
                return result
        for size, accepted in ((7424, True), (7423, False), (7425, False), (100000, False)):
            data = bytes(artifact())
            data = data[:size] if size <= len(data) else data + bytes(size-len(data))
            stream = Spy(data)
            output = io.StringIO()
            with self.subTest(size=size), mock.patch.object(sys, "argv", ["decoder", "synthetic.bin"]), \
                 mock.patch.object(Path, "open", return_value=stream) as opened, contextlib.redirect_stdout(output):
                if accepted:
                    decoder.main()
                    self.assertIn("committed=0", output.getvalue())
                else:
                    with self.assertRaises(ValueError):
                        decoder.main()
                    self.assertEqual(output.getvalue(), "")
                opened.assert_called_once_with("rb")
            self.assertEqual(stream.requests, [7425])
            self.assertLessEqual(sum(stream.returned), 7425)
            self.assertLessEqual(len(output.getvalue().encode()), 24000)


if __name__ == "__main__":
    unittest.main()
