import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location(
    "semaphore_decoder_under_test",
    Path(__file__).resolve().parents[1] / "tools" / "decode_semaphore_provenance.py")
d = importlib.util.module_from_spec(spec)
spec.loader.exec_module(d)


def fixture():
    data = bytearray(d.SIZE)
    struct.pack_into("<10Q", data, 0, d.MAGIC, 1, d.SIZE, 123, 1, 300, 1,
                     0x910000000, 0x2409000, 7)
    data[80:112] = bytes.fromhex(d.HASH)
    return data


def record(data, bank, within=0, complete=True):
    index = bank * d.PER_BANK + within
    current = struct.unpack_from("<Q", data, 112 + bank * 8)[0]
    struct.pack_into("<Q", data, 112 + bank * 8, max(current, within + 1))
    w = [0] * 32
    w[0:7] = [bank % 3 + 1, index * 2 + 1, index * 2 + 2, 0x9106f65ca, 555, 0, 1]
    w[7:13] = [0xffffffffffffffff, 1, 0, 0, 0, 0]
    w[25:28] = [0x80020003, 79, 79]
    struct.pack_into("<34Q", data, d.HEADER + index * d.SLOT,
                     index + 1, index + 1 if complete else 0, *w)
    return d.HEADER + index * d.SLOT + 16


class DecodeTests(unittest.TestCase):
    def test_invalid_poll_has_result_but_no_object(self):
        data = fixture()
        record(data, 2)
        row = d.decode(data)["records"][0]
        self.assertEqual(row["result"], 0x80020003)
        self.assertIsNone(row["object"])
        self.assertEqual(row["errno_after"], 79)
        self.assertTrue(all(v is None for v in row["before"].values()))

    def test_entry_does_not_invent_completion(self):
        data = fixture()
        record(data, 1, complete=False)
        row = d.decode(data)["records"][0]
        self.assertIsNone(row["result"])
        self.assertIsNone(row["after"])
        self.assertIsNone(row["errno_after"])

    def test_negative_appid_and_sentinel_are_distinct_from_failed_read(self):
        data = fixture()
        offset = record(data, 0)
        struct.pack_into("<6Q", data, offset + 13 * 8, 5, 0xffffffff, 0,
                         0xffffffffffffffff, 0, 0)
        row = d.decode(data)["records"][0]
        self.assertEqual(row["before"]["appId"], -1)
        self.assertEqual(row["before"]["sema"], 0xffffffffffffffff)
        self.assertIsNone(row["before"]["event"])

    def test_reject_fabricated_poll_object(self):
        data = fixture()
        offset = record(data, 2)
        struct.pack_into("<Q", data, offset + 5 * 8, 0x1234)
        with self.assertRaisesRegex(ValueError, "attribution forbidden"):
            d.decode(data)

    def test_bad_size_commit_and_profile(self):
        for mutation in ("size", "commit", "hash"):
            data = fixture()
            record(data, 0)
            if mutation == "size":
                data += b"x"
            elif mutation == "commit":
                struct.pack_into("<Q", data, d.HEADER, 999)
            else:
                data[80] ^= 1
            with self.assertRaises(ValueError):
                d.decode(data)

    def test_full_buffer_and_binary_name_have_bounded_text(self):
        data = fixture()
        for bank in range(6):
            for within in range(d.PER_BANK):
                offset = record(data, bank, within)
                struct.pack_into("<Q", data, offset + 13 * 8, 32)
                data[offset + 224:offset + 256] = b"\xff" * 32
        decoded = d.decode(data)
        self.assertEqual(len(decoded["records"]), d.COUNT)
        text = d.render(decoded)
        self.assertLessEqual(len(text.encode("utf-8")), d.MAX_TEXT)
        self.assertIn("text_cap_omitted=", text)

    def test_reserved_but_unpublished_is_reported(self):
        data = fixture()
        struct.pack_into("<Q", data, 112, 1)
        self.assertEqual(d.decode(data)["uncommitted_reservations"], 1)


if __name__ == "__main__":
    unittest.main()
