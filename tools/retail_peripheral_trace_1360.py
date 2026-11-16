"""Read-only decoders for retail initial-onboarding query hardware probes.

All offsets are from the original 13.60 AOT, not the mixed JS decompilation.
No process access here. A completed native list is not proof of JS acceptance.
"""
import struct

from home_gate_trace_1360 import DecodeError, require

SHA256 = '8892e5107bb68c917fbf952cdc11978292ed1bbfa82797364cda14dde9083afa'
CORLIB_SHA256 = '4412a22d53f4f80e1a8028a1d57f22f4dd006c5143c5154c7a4ea8753c0eefb8'
NAME = 'Sce.Vsh.ShellUI.ReactNativeShellApp.dll.sprx'
SITES = {'entry': 0x1a6650, 'enumerated': 0x1a6853, 'converted': 0x1a6a2d,
         'results': 0x1a6af9, 'owner': 0x1a6967, 'allowed': 0x1a6a91}
# These include call destinations, frame offsets and loop stride, not just prologues.
ABI = {
    0x1a6650: '55488bec4881ecf001000048895dd84c',
    0x1a684e: 'e80d870b00488b3df64d4f00',
    0x1a68f3: '4c8bf84c8ba5b8feffff33db',
    0x1a6908: '4863c341394424180f8667020000486bc070498bf44803f04883c620488b',
    0x1a6938: '488b85e0feffff4883f8ff0f8467010000486385e8feffff3dff0000000f84550100004863bde8feffffe851850b00480fb6c085c0',
    0x1a6a28: 'e84f850b00480fb6c085c00f84770000004585ed',
    0x1a6a8c: 'e8ff740c00480fb6c085c00f8413000000',
    0x1a6ae9: '488b3528a34f00488bf8498bd7833800e856c30a00',
    0x1a68d5: '4883c23848890ac1',
}
TYPES = {'controller', 'standardController', 'leonardo', 'specialController',
         'move', 'vrController', 'headset', 'keyboard', 'mouse', 'remoteController'}


def audit(image, corlib):
    for address, raw in ABI.items():
        require(image.at(address, len(bytes.fromhex(raw))) == bytes.fromhex(raw), 'peripheral_abi')
    # Retail List<T>.get_Count: the size field is +0x20, NOT +0x18.
    require(corlib.at(0x27cb60, 21) == bytes.fromhex('554889e548897df848897df0488b45f88b40205dc3'), 'retail_list_count_abi')


def device(reader, p):
    raw = reader.raw(p, 112)
    # Deliberately exclude name, Bluetooth address and other device strings.
    return dict(device_id=struct.unpack_from('<q', raw)[0],
                owner=struct.unpack_from('<i', raw, 8)[0],
                vendor=struct.unpack_from('<H', raw, 12)[0],
                product=struct.unpack_from('<H', raw, 14)[0],
                connection=struct.unpack_from('<i', raw, 16)[0],
                native_type=struct.unpack_from('<q', raw, 40)[0],
                native_subtype=struct.unpack_from('<q', raw, 48)[0],
                enabled_functions=struct.unpack_from('<Q', raw, 56)[0],
                builtin_functions=struct.unpack_from('<Q', raw, 64)[0])


def device_array(reader, p):
    require(reader.q(p + 16) == 0, 'peripheral_array_rank')
    count = reader.q(p + 24)
    require(count <= 256, 'peripheral_array_count')
    return [device(reader, p + 32 + j * 112) for j in range(count)]


def result_count(reader, p):
    # JArray's constructor in this worker installs List<JToken> at +0x38.
    items = reader.q(p + 56)
    count = reader.i(items + 32)
    array = reader.q(items + 16)
    require(reader.q(array + 16) == 0, 'peripheral_result_rank')
    require(0 <= count <= reader.q(array + 24) <= 256, 'peripheral_result_count')
    return count


class Model:
    phase = 'retail_peripheral'
    ready = False

    def __init__(self, emit, bases):
        self.emit, self.pending, self.next_id = emit, {}, 0

    def thread_boundary(self, tid):
        for key in [k for k in self.pending if k[0] == tid]:
            self.emit(dict(event='query_incomplete', query=self.pending.pop(key), reason='thread_boundary'))

    def close_coverage(self, now, reason):
        self.emit(dict(event='coverage_closed', reason=reason, pending=list(self.pending.values())))

    def outstanding(self, now):
        return list(self.pending.values())

    def hit(self, name, reader, regs, now, tid):
        # Entry is before push rbp; after prologue this invocation's rbp=rsp-8.
        frame = regs['rsp'] - 8 if name == 'entry' else regs['rbp']
        key = (tid, frame)
        if name == 'entry':
            if key in self.pending:
                self.emit(dict(event='query_incomplete', query=self.pending.pop(key), reason='frame_reused'))
            require(len(self.pending) < 256, 'peripheral_pending_budget')
            self.next_id += 1
            self.pending[key] = self.next_id
            self.emit(dict(event='query_entry', query=self.next_id, tid=tid))
            return
        query = self.pending.get(key)
        row = dict(event=name, query=query, entry_observed=query is not None, tid=tid)
        if name == 'enumerated':
            row.update(native_return=struct.unpack('<i', struct.pack('<I', regs['rax'] & 0xffffffff))[0],
                       initial_onboarding=bool(regs['r13'] & 0xffffffff))
            try:
                row['devices'] = device_array(reader, reader.q(frame - 0x148))
            except DecodeError:
                # Preserve the actual error return even when its output is invalid.
                self.emit(dict(row, devices_unknown=True))
                raise
        elif name in ('converted', 'owner'):
            row.update(index=regs['rbx'] & 0xffffffff, device=device(reader, frame - 0x120))
            row['conversion_ok' if name == 'converted' else 'valid_available_owner'] = bool(regs['rax'] & 255)
        elif name == 'allowed':
            p = reader.q(frame - 0x160)
            size = reader.i(p + 16)
            require(0 <= size <= 128, 'peripheral_type_length')
            text = reader.raw(p + 20, size * 2).decode('utf-16-le', errors='strict')
            row.update(index=regs['rbx'] & 0xffffffff, allowed=bool(regs['rax'] & 255),
                       peripheral_type=text if text in TYPES else 'other')
        elif name == 'results':
            row.update(result_count=result_count(reader, regs['rdx']),
                       initial_onboarding=bool(regs['r13'] & 0xffffffff))
            self.pending.pop(key, None)
        else:
            raise ValueError('unknown peripheral probe')
        self.emit(row)


class Observer:
    def __init__(self, mode):
        self.mode = 'retail_peripheral_' + mode
        names = ('entry', 'enumerated', 'converted', 'results') if mode == 'query' else ('entry', 'enumerated', 'owner', 'allowed')
        require(mode in ('query', 'filters'), 'peripheral_mode')
        self.probes = {n: ('peripheral', SITES[n]) for n in names}
        self.windows = {'peripheral': [(a, max(8, len(bytes.fromhex(b)))) for a, b in ABI.items()] +
                                     [(SITES[n], 8) for n in names]}

    def model(self, emit, bases):
        return Model(emit, bases)
