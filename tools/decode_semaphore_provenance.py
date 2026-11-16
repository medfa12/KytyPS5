"""Decode a stopped/exited-process SMPROV1 snapshot, with a 64-KiB text ceiling.

Never reads process memory or executes guest code. Unknown Poll objects remain
unknown; no handle-based join, temporal attribution, or initializer inference.
"""
import argparse
import struct
from pathlib import Path

MAGIC = 0x0031564F52504D53
SIZE, HEADER, SLOT, COUNT, PER_BANK = 39424, 256, 272, 144, 24
MAX_TEXT = 65536
HASH = "6870113e176333d95f1241864af35c20ac7115c23948e002da0ebaec4819250a"
EVENTS = ("CreateEvent", "CreateSema", "PollSema")
PIN_STATES = ("unseen", "verified", "unloaded", "rejected-or-duplicate")


def snapshot(words, name=False):
    mask = words[0]
    if mask & ~(63 if name else 31):
        raise ValueError("unknown snapshot validity bits")
    values = [words[i + 1] if mask & (1 << i) else None for i in range(5)]
    if values[0] is not None:
        values[0] = struct.unpack("<i", struct.pack("<I", values[0] & 0xffffffff))[0]
    return dict(zip(("appId", "event", "sema", "locked", "output"), values))


def decode(data):
    if len(data) != SIZE:
        raise ValueError(f"expected exactly {SIZE} bytes; not an SMPROV1 snapshot")
    head = struct.unpack_from("<10Q", data)
    magic, version, size, pid, ready, sequence, pin, base, image_size, program_id = head
    if (magic, version, size, ready) != (MAGIC, 1, SIZE, 1) or pin >= len(PIN_STATES):
        raise ValueError("invalid/unready header")
    if data[80:112].hex() != HASH:
        raise ValueError("unexpected fingerprint profile")
    reserved = struct.unpack_from("<6Q", data, 112)
    dropped = struct.unpack_from("<6Q", data, 160)
    if any(n > PER_BANK for n in reserved):
        raise ValueError("reservation count exceeds fixed bank capacity")
    records = []
    uncommitted = 0
    for index in range(COUNT):
        offset = HEADER + index * SLOT
        entry, exit_ = struct.unpack_from("<2Q", data, offset)
        bank, within = divmod(index, PER_BANK)
        if not entry:
            if exit_:
                raise ValueError("exit without entry")
            uncommitted += within < reserved[bank]
            continue
        if entry != index + 1 or exit_ not in (0, index + 1) or within >= reserved[bank]:
            raise ValueError("invalid commit stamp/reservation")
        w = struct.unpack_from("<32Q", data, offset + 16)
        event, begin, end, caller, tid, obj, state = w[:7]
        if event != bank % 3 + 1 or not begin or state >= len(PIN_STATES):
            raise ValueError("invalid committed event")
        if bank < 3 and state != 1:
            raise ValueError("pinned bank without verified placement")
        if obj and (event == 3 or bank >= 3):
            raise ValueError("object attribution forbidden for Poll/unpinned callers")
        if exit_ and (end <= begin or w[25] > 0xffffffff):
            raise ValueError("invalid result/completion sequence")
        raw_name = data[offset + 16 + 224:offset + 16 + 256]
        records.append({
            "slot": index, "event": EVENTS[event - 1], "pinned": bank < 3,
            "begin": begin, "end": end if exit_ else None, "caller": caller,
            "tid": tid, "object": obj or None, "args": w[7:13],
            "before": snapshot(w[13:19], name=True),
            "after": snapshot(w[19:25]) if exit_ else None,
            "result": w[25] if exit_ else None, "errno_before": w[26],
            "errno_after": w[27] if exit_ else None,
            "name": raw_name.split(b"\0", 1)[0] if w[13] & 32 else None,
        })
    records.sort(key=lambda row: row["begin"])
    return {"pid": pid, "pin": PIN_STATES[pin], "base": base, "image_size": image_size,
            "program_id": program_id, "sequence": sequence, "reserved": reserved,
            "dropped": dropped, "uncommitted_reservations": uncommitted, "records": records}


def value(number):
    return "?" if number is None else hex(number)


def render(decoded):
    lines = [f"SMPROV1 pid={decoded['pid']} pin={decoded['pin']} base={value(decoded['base'])} "
             f"size={value(decoded['image_size'])} module={decoded['program_id']}\n",
             f"profile_sha256={HASH}\n",
             f"reserved={decoded['reserved']} dropped={decoded['dropped']} "
             f"uncommitted_reservations={decoded['uncommitted_reservations']}\n",
             "Order=entry reservation sequence, not completion order. '?'=unknown/read failed. "
             "Snapshots are non-atomic. Entry-only is not an API result.\n",
             "Poll object is UNKNOWN. No initializer entry/exit capture or sentinel-handle join.\n"]
    size = len("".join(lines).encode("utf-8"))
    emitted = 0
    for r in decoded["records"]:
        before = ",".join(f"{k}={value(v)}" for k, v in r["before"].items())
        after = "unfinished" if r["after"] is None else ",".join(f"{k}={value(v)}" for k, v in r["after"].items())
        line = (f"{r['begin']}/{r['end']} {r['event']} pinned={int(r['pinned'])} "
                f"tid={r['tid']} ra={value(r['caller'])} object={value(r['object'])} "
                f"args={','.join(map(hex, r['args']))} result={value(r['result'])} "
                f"errno={value(r['errno_before'])}->{value(r['errno_after'])} "
                f"pre[{before}] post[{after}] name={r['name']!r}\n")
        if size + len(line.encode("utf-8")) > MAX_TEXT - 200:
            break
        lines.append(line)
        size += len(line.encode("utf-8"))
        emitted += 1
    lines.append(f"decoded_records={emitted} text_cap_omitted={len(decoded['records']) - emitted}; "
                 "omitted text remains in binary; capture drops are separate.\n")
    text = "".join(lines)
    assert len(text.encode("utf-8")) <= MAX_TEXT
    return text


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("snapshot", type=Path)
    args = parser.parse_args()
    # Read at most one byte beyond the exact schema, even for a hostile huge file.
    with args.snapshot.open("rb") as stream:
        result = decode(stream.read(SIZE + 1))
    print(render(result), end="")
