"""Offline decoder only. Read a completed, stopped-process probe artifact.

No debugger, process reads, polling, network, or emulation is performed.
Binary artifact: 7,424 bytes. Text: <=24,000 bytes. Combined: <=31,424.
"""
import argparse
from pathlib import Path
import struct
import sys

MAGIC = 0x0031564F52505053
SIZE = 7424
CAPS = (32, 8, 8, 8)
OFFSETS = (0, 32, 40, 48)
EVENTS = {1: "create", 2: "publish", 3: "close", 4: "delete", 5: "read-dispatch", 6: "socketpair"}
KINDS = {0: "file", 1: "random", 2: "console", 3: "hid", 4: "notification", 5: "pipe", 6: "npdrm", 7: "anonymous-shm"}


def signed64(n):
    return n - (1 << 64) if n & (1 << 63) else n


def pair(n):
    return struct.unpack("<ii", struct.pack("<Q", n))


def decode(data):
    if len(data) != SIZE:
        raise ValueError("expected exactly 7424 bytes")
    h = struct.unpack_from("<32Q", data)
    if (h[0], h[1], h[3], h[4]) != (MAGIC, 1, SIZE, 1) or tuple(h[8:12]) != CAPS:
        raise ValueError("uninitialized or unsupported probe header")
    reservations, dropped = h[12:16], h[16:20]
    lines = [f"SPPROV1 pid={h[2]} sequence={h[5]} generations={h[6]}",
             f"banks=life,pair,notification-read,other-read reserved={reservations} dropped={dropped}"]
    records = []
    incomplete = []
    for bank, (base, capacity) in enumerate(zip(OFFSETS, CAPS)):
        if reservations[bank] > capacity:
            raise ValueError("reservation count exceeds bank capacity")
        for local in range(reservations[bank]):
            index = base + local
            commit, *r = struct.unpack_from("<16Q", data, 256 + index * 128)
            if commit == 0:
                incomplete.append(index)
                continue
            if commit != index + 1:
                raise ValueError("invalid slot commit")
            if r[1] not in EVENTS or (bank == 0 and r[1] > 4) or (bank == 1 and r[1] != 6) or (bank > 1 and r[1] != 5):
                raise ValueError("invalid bank/event combination")
            records.append(r)
    for r in sorted(records, key=lambda item: item[0]):
        seq, event, caller, tid, context, fd, obj, generation, kind, *d = r
        common = f"seq={seq} {EVENTS[event]} ra=0x{caller:016x} tid={tid} ctx=0x{context:016x}"
        if event == 6:
            before = str(pair(d[4])) if kind & 1 else "unreadable"
            after = str(pair(d[5])) if kind & 2 else "unreadable"
            lines.append(common + f" args=0x{d[0]:x},0x{d[1]:x},0x{d[2]:x} out=0x{obj:016x}"
                         f" ret=0x{d[3]:016x} pre={before} post={after} unchanged={bool(kind & 4)} flags=0x{kind:x}")
        else:
            lines.append(common + f" fd={signed64(fd)} obj=0x{obj:016x} gen={generation}"
                         f" kind={KINDS.get(kind, str(kind))} opened={d[0]} gls={d[1]}"
                         f" creator_ctx=0x{d[2]:016x} request={d[3]}")
    lines.append(f"committed={len(records)} incomplete_slots={incomplete}; missing or dropped records are inconclusive")
    text = "\n".join(lines) + "\n"
    if len(text.encode("utf-8")) > 24000:
        raise ValueError("decoded output exceeds fixed text budget")
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact", type=Path)
    args = parser.parse_args()
    # Bounded input even if the supplied file is not a probe artifact.
    with args.artifact.open("rb") as stream:
        data = stream.read(SIZE + 1)
    sys.stdout.write(decode(data))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError):
        sys.stderr.write("socketpair provenance: invalid, incomplete, or unreadable artifact\n")
        raise SystemExit(1)
