"""Decode the version-1 bounded IPMI observer file (stdlib only)."""

import argparse
import json
import struct
from pathlib import Path

CAPACITIES = (256, 256, 512, 64, 128)
OFFSETS = (0, 256, 512, 1024, 1088)
BANKS = ("lifecycle", "invoke", "recovery", "poll", "gateway")
EVENTS = (
    "invalid", "client_create", "server_create", "connect_enter", "connect_exit",
    "disconnect", "destroy", "invoke_enter", "invoke_exit", "raw_enter", "raw_exit",
    "input", "dispatcher_enter", "dispatcher_exit", "poll_enter", "poll_exit",
    "gateway_enter", "gateway_exit",
    "psn_failure", "np_memory_result", "np_allocator_result", "np_mutex_query", "np_memory_callback", "np_heap", "stack_failure", "app_id_result", "np_global_result", "net_pool", "ssl_context",
)
FLAGS = {
    1: "name_readable", 2: "vtable_readable", 4: "descriptor_readable",
    8: "result_readable", 16: "preview_readable", 32: "preview_unchanged",
    64: "recovery_target", 128: "raw_written_readable",
    256: "mutex_byte_readable",
}


def decode(data, recovery_only=False):
    if len(data) != 311552:
        raise ValueError(f"expected 311552 bytes, got {len(data)}")
    magic, version, pid, frequency, start, calls, ready = struct.unpack_from("<7Q", data)
    if magic != 0x3142534F494D5049 or version != 1 or ready != 1:
        raise ValueError("unsupported or uninitialized IPMI observer file")
    capacities = struct.unpack_from("<5Q", data, 56)
    if capacities != CAPACITIES:
        raise ValueError("unexpected bank capacities")
    attempted = struct.unpack_from("<5Q", data, 96)
    dropped = struct.unpack_from("<5Q", data, 136)
    records = []
    holes = dict.fromkeys(BANKS, 0)
    for bank, offset, capacity in zip(BANKS, OFFSETS, capacities):
        for i in range(min(attempted[BANKS.index(bank)], capacity)):
            slot = offset + i
            position = 256 + slot * 256
            commit, event, tick, call = struct.unpack_from("<4Q", data, position)
            if commit != slot + 1:
                holes[bank] += 1
                continue
            if recovery_only and bank != "recovery":
                continue
            values = struct.unpack_from("<16Q", data, position + 32)
            record = {
                "slot": slot, "bank": bank,
                "event": EVENTS[event] if event < len(EVENTS) else event,
                "call": call, "seconds": (tick - start) / frequency if frequency else None,
                "host_tid": struct.unpack_from("<Q", data, position + 216)[0],
                "name": data[position + 160:position + 192].split(b"\0", 1)[0].decode("ascii", "replace"),
                "values": [f"0x{x:x}" for x in values],
                "flags": [name for bit, name in FLAGS.items() if values[14] & bit],
            }
            if event in (7, 8, 9, 10):
                record.update(client=f"0x{values[0]:x}", caller=f"0x{values[1]:x}",
                              vtable=f"0x{values[2]:x}", method=f"0x{values[3]:x}",
                              input_count=values[5], output_count=values[7])
                if values[14] & 4:
                    record.update(output=f"0x{values[9]:x}", capacity=values[10])
                    if event in (7, 8) or values[14] & 128:
                        record["written"] = values[11]
                if event in (9, 10):
                    record["raw_written_pointer"] = f"0x{values[15]:x}"
                if values[14] & 8:
                    record["method_result"] = f"0x{values[12]:08x}"
                if event in (8, 10):
                    record["transport"] = f"0x{values[13]:08x}"
                if values[14] & 16:
                    preview = data[position + 192:position + 216]
                    record["preview_hex"] = preview.hex()
                    record["phase_u32_at_0x14"] = struct.unpack_from("<I", preview, 20)[0]
            elif event == 18:
                record.update(caller=f"0x{values[0]:x}", arguments=[f"0x{x:x}" for x in values[1:7]],
                              format_offset=values[7], terminated=bool(values[15]))
                if values[12]:
                    record.update(stage_mask=f"0x{values[8]:x}", syndy_service=f"0x{values[9]:x}")
                if values[13]:
                    record.update(create_binding=f"0x{values[10]:x}", allocator_binding=f"0x{values[11]:x}")
                if values[14] & 16:
                    record["format_chunk"] = data[position + 192:position + 216].split(b"\0", 1)[0].decode("utf-8", "replace")
            elif event == 19:
                record.update(caller=f"0x{values[0]:x}", size=values[1], name_pointer=f"0x{values[2]:x}")
                if values[15] == 1:
                    record.update(abi="status", supplied_buffer=f"0x{values[3]:x}", mode=values[4],
                                  status=f"0x{values[5]:08x}")
                else:
                    record.update(abi="legacy_pointer", a2=f"0x{values[3]:x}",
                                  out=f"0x{values[4]:x}", result=f"0x{values[5]:x}")
            elif event == 20:
                record.update(caller=f"0x{values[0]:x}", allocator=f"0x{values[1]:x}")
            elif event == 22:
                record.update(caller=f"0x{values[0]:x}", operation={0: "alloc", 1: "realloc", 2: "free"}.get(values[1], values[1]),
                              size=values[2], pointer=f"0x{values[3]:x}", context=f"0x{values[4]:x}",
                              result=f"0x{values[5]:x}", diagnostic_count=values[6])
            elif event == 23:
                record.update(caller=f"0x{values[0]:x}",
                              operation={0: "init", 1: "destroy", 2: "descriptor", 3: "descriptor_ex",
                                         4: "alloc", 5: "realloc", 6: "free"}.get(values[1], values[1]),
                              subject=f"0x{values[2]:x}", handle=f"0x{values[3]:x}", size=values[4],
                              input=f"0x{values[5]:x}", result=f"0x{values[6]:x}", status=f"0x{values[7]:08x}",
                              aux=f"0x{values[8]:x}")
            elif event == 28:
                record.update(caller=f"0x{values[0]:x}",
                              operation={0: "init", 1: "term", 2: "occupied"}.get(values[1], values[1]),
                              ssl_id=values[2] if values[2] < 0x80000000 else values[2] - 0x100000000)
                if values[1] in (0, 1):
                    record["result"] = f"0x{values[3]:08x}"
                if values[1] in (0, 2):
                    record["size"] = values[4]  # Full unsigned64 value, never the pool event's signed32 conversion.
            elif event == 27:
                record.update(caller=f"0x{values[0]:x}",
                              operation={0: "create", 1: "destroy", 2: "occupied", 3: "config"}.get(values[1], values[1]),
                              pool_id=values[2] if values[2] < 0x80000000 else values[2] - 0x100000000)
                if values[1] in (0, 1, 3):
                    record["result"] = f"0x{values[3]:08x}"
                if values[1] in (0, 2):
                    record["size"] = values[4] if values[4] < 0x80000000 else values[4] - 0x100000000
                if values[1] == 0:
                    record.update(pool_flags=f"0x{values[5]:08x}", name_pointer=f"0x{values[6]:x}")
                if values[1] == 3:
                    record.update(config_pointer=f"0x{values[6]:x}", config_readable=bool(values[14] & 4))
                    if values[14] & 4:
                        record.update(config_flags=f"0x{values[4]:08x}",
                                      config_hex=struct.pack('<4Q', *values[8:12]).hex())
            elif event == 26:
                record.update(caller=f"0x{values[0]:x}", operation="get_ex" if values[1] else "get",
                              descriptor=f"0x{values[2]:x}", heap=f"0x{values[3]:x}", size=values[4],
                              mapping=f"0x{values[5]:x}", handle=f"0x{values[6]:x}", error=f"0x{values[7]:08x}")
            elif event == 25:
                record.update(caller=f"0x{values[0]:x}", pid=values[1], output=f"0x{values[2]:x}",
                              status=f"0x{values[3]:08x}", before_readable=bool(values[4]), after_readable=bool(values[5]),
                              before_u32=list(values[6:9]), after_u32=list(values[9:12]))
            elif event == 24:
                record.update(caller=f"0x{values[0]:x}", address=f"0x{values[1]:x}", chunk=values[2])
                record["flags"] = []
                if values[2] == 0:
                    record["guard"] = f"0x{values[3]:x}"
                else:
                    record["qwords"] = [f"0x{x:x}" if values[3] & (1 << i) else None
                                        for i, x in enumerate(values[4:])]
            elif event == 21:
                record.update(mutex=f"0x{values[0]:x}", caller=f"0x{values[1]:x}",
                              result=values[2])
                if values[14] & 256:
                    record["guest_initialized_byte"] = values[3]
            records.append(record)
    records.sort(key=lambda r: (r["seconds"] or 0, r["slot"]))
    return {"version": version, "pid": pid, "calls": calls,
            "attempted": dict(zip(BANKS, attempted)), "dropped": dict(zip(BANKS, dropped)),
            "unpublished_slots": holes, "records": records}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--recovery-only", action="store_true")
    args = parser.parse_args()
    try:
        result = decode(args.capture.read_bytes(), args.recovery_only)
    except ValueError as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
