"""Bounded read-only ShellUI 13.60 background/shader snapshot (Windows x64)."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import os
from pathlib import Path
import struct
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--base', type=lambda v: int(v, 0), default=0x930000000)
    parser.add_argument('--shellcore-base', type=lambda v: int(v, 0), default=0x910000000)
    parser.add_argument('--bglayer-base', type=lambda v: int(v, 0), default=0xd20000000)
    parser.add_argument('--np-descriptor', type=lambda v: int(v, 0), default=0,
                        help='Observed GetAllocator result; read-only optional snapshot')
    parser.add_argument('--image', type=Path, default=Path(__file__).resolve().parents[1] / '_Build/windows/kyty_emulator.exe')
    args = parser.parse_args()
    if C.sizeof(C.c_void_p) != 8 or args.pid <= 0:
        parser.error('Requires x64 Python and a positive target PID')
    kernel = C.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
    kernel.OpenProcess.restype = W.HANDLE
    kernel.CloseHandle.argtypes = [W.HANDLE]
    kernel.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
    kernel.QueryFullProcessImageNameW.argtypes = [W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)]
    process = kernel.OpenProcess(0x1010, False, args.pid)
    if not process:
        raise C.WinError(C.get_last_error())
    image = C.create_unicode_buffer(32768)
    image_length = W.DWORD(len(image))
    if not kernel.QueryFullProcessImageNameW(process, 0, image, C.byref(image_length)):
        kernel.CloseHandle(process)
        raise C.WinError(C.get_last_error())
    if os.path.normcase(os.path.realpath(image.value)) != os.path.normcase(str(args.image.resolve())):
        kernel.CloseHandle(process)
        raise ValueError('Target image differs from expected executable')
    bytes_read = 0

    def read(address, length):
        nonlocal bytes_read
        if not address or not 0 < length <= 4096 or bytes_read + length > 32768:
            raise ValueError('Invalid bounded read')
        bytes_read += length
        buf = C.create_string_buffer(length)
        count = C.c_size_t()
        if not kernel.ReadProcessMemory(process, address, buf, length, C.byref(count)) or count.value != length:
            raise C.WinError(C.get_last_error())
        return buf.raw

    def q(address):
        return int.from_bytes(read(address, 8), 'little')

    def shader(obj):
        header = q(obj + 0x60)
        raw = read(header, 0x60)
        magic, version = struct.unpack_from('<II', raw)
        if (magic, version) != (0x34333231, 0x18):
            raise ValueError('Unexpected native shader header')
        regs = q(header + 0x20)
        count = raw[0x5c]
        if count > 128:
            raise ValueError('Shader register count exceeds bound')
        pairs = dict(struct.iter_unpack('<II', read(regs, count * 8))) if count else {}
        programs = {}
        for stage, offset in [('PS', 0x08), ('ES', 0xc8), ('GS', 0x88), ('LS', 0x148), ('HS', 0x108)]:
            if offset in pairs and offset + 1 in pairs:
                programs[stage] = hex((pairs[offset] << 8) | ((pairs[offset + 1] & 255) << 40))
        code = struct.unpack_from('<Q', raw, 0x10)[0]
        code_size = struct.unpack_from('<I', raw, 0x44)[0]
        return dict(object=hex(obj), header=hex(header), raw=raw.hex(), programs=programs,
                    reflection=hex(q(obj + 0x70)), clone_flag=read(obj + 0x78, 1)[0],
                    code=hex(code), code_size=code_size,
                    code_prefix=read(code, min(code_size, 256)).hex() if code_size else '')

    def transition(renderer):
        target = q(renderer + 0xd8)
        raw = read(target, 0x1d8)
        if struct.unpack_from('<Q', raw)[0] != args.base + 0x116a0e0 or raw[0x38:0x42] != b'Transition':
            raise ValueError('Unexpected Transition identity')
        field = lambda offset: struct.unpack_from('<Q', raw, offset)[0]
        helper = field(0xd8)
        entry = dict(pointer=hex(target), raw=raw.hex(), selector=struct.unpack_from('<I', raw, 0xd0)[0],
                     opacity=struct.unpack_from('<f', raw, 0x58)[0], helper=hex(helper),
                     helper_hex=read(helper, 0xf8).hex() if helper else None,
                     images={hex(offset): hex(field(offset)) for offset in range(0x168, 0x1a0, 8)},
                     shaders={}, constants={})
        for name, offset in [('vs', 0x20), ('ripple', 0xe8), ('slide', 0xf0), ('cross_fade', 0xf8),
                             ('game_splash', 0x100), ('single', 0x108), ('ripple_logo', 0x110)]:
            wrapper = field(offset)
            try:
                entry['shaders'][name] = dict(wrapper=hex(wrapper), shader=shader(q(wrapper + 0x18)))
            except (OSError, ValueError) as error:
                entry['shaders'][name] = dict(wrapper=hex(wrapper), error=str(error))
        for offset, length in zip(range(0x1a0, 0x1d0, 8), [0x28, 8, 0x18, 8, 0x10, 4]):
            buffer = field(offset)
            try:
                descriptor = read(buffer + 0x58, 6)
                payload = int.from_bytes(descriptor, 'little')
                entry['constants'][hex(offset)] = dict(buffer=hex(buffer), payload=hex(payload), hex=read(payload, length).hex())
            except (OSError, ValueError) as error:
                entry['constants'][hex(offset)] = dict(buffer=hex(buffer), error=str(error))
        entry['root_stable'] = q(renderer + 0xd8) == target
        return entry

    result = dict(pid=args.pid, base=hex(args.base), non_atomic=True, samples=[])
    try:
        core = dict(base=hex(args.shellcore_base), roots={})
        result['shellcore_preparation'] = core
        if args.np_descriptor:
            try:
                descriptor = read(args.np_descriptor, 32)
                core['np_descriptor'] = dict(pointer=hex(args.np_descriptor),
                                             words=[hex(word[0]) for word in struct.iter_unpack('<Q', descriptor)])
            except (OSError, ValueError) as error:
                core['np_descriptor'] = dict(pointer=hex(args.np_descriptor), error=str(error))
        for name, offset in [('manager', 0x2156888), ('companion', 0x2156890),
                             ('server_owner', 0x2156ae0), ('recovery_owner', 0x226d298),
                             ('startup_service', 0x2156570), ('save_data_manager', 0x20b42b8)]:
            try:
                pointer = q(args.shellcore_base + offset)
                entry = dict(pointer=hex(pointer))
                core['roots'][name] = entry
                if pointer and name == 'startup_service':
                    entry['members'] = {hex(offset): hex(q(pointer + offset)) for offset in (8, 0x10, 0x18, 0x20)}
                if pointer and name == 'save_data_manager':
                    watchdog = q(pointer + 0x40)
                    entry['watchdog'] = hex(watchdog)
                    if watchdog:
                        entry['watchdog_stop'] = read(watchdog + 0x22, 1)[0]
                if pointer and name == 'server_owner':
                    server, handler = struct.unpack('<QQ', read(pointer + 0x1000, 16))
                    entry.update(server=hex(server), handler=hex(handler),
                                 thread=hex(q(pointer + 0x1040)))
                    if server:
                        vtable, kid = struct.unpack('<Qi', read(server, 12))
                        entry.update(server_vtable=hex(vtable), server_kid=kid)
                entry['root_stable'] = q(args.shellcore_base + offset) == pointer
            except (OSError, ValueError) as error:
                core['roots'][name] = dict(error=str(error))
        try:
            core['phase'] = struct.unpack('<i', read(args.shellcore_base + 0x226d540, 4))[0]
            core['np_stage_mask'] = hex(q(args.shellcore_base + 0x21578d8))
            core['app_cache_mutex_hex'] = read(args.shellcore_base + 0x21594b8, 0x18).hex()
            core['np_mutex_bindings'] = {name: hex(q(args.shellcore_base + offset))
                                        for name, offset in [('ctor', 0x208d800), ('is_init', 0x208d810), ('init', 0x208d7e0)]}
            core['np_array_bindings'] = {name: hex(q(args.shellcore_base + offset))
                                        for name, offset in [('new', 0x208d9b0), ('delete', 0x208d9a0)]}
            core['np_worker_manager_hex'] = read(args.shellcore_base + 0x2160de8, 0x20).hex()
            core['np_workqueue_bindings'] = {name: hex(q(args.shellcore_base + offset))
                                           for name, offset in [('ctor', 0x208dca8), ('init', 0x208dc58), ('start', 0x208dc68)]}
            core['np_push_heap_hex'] = read(args.shellcore_base + 0x2162cd0, 0x20).hex()
            core['np_push_allocator_hex'] = read(args.shellcore_base + 0x2162cf0, 0x20).hex()
            core['np_push_allocator_ex_hex'] = read(args.shellcore_base + 0x2162d10, 0x28).hex()
            core['np_push_heap_bindings'] = {name: hex(q(args.shellcore_base + offset))
                                           for name, offset in [('init', 0x208d1e8), ('allocator', 0x208d1f8), ('allocator_ex', 0x208d200)]}
        except (OSError, ValueError) as error:
            core['phase_error'] = str(error)
        try:
            # Inline manager, not a pointer root. Count is published before allocation.
            first = q(args.shellcore_base + 0x2160df8)
            count = struct.unpack('<i', read(args.shellcore_base + 0x2160df0, 4))[0]
            workers = dict(first=hex(first), count=count)
            core['np_workers'] = workers
            if first:
                if first < 0x18 or not 0 < count <= 5:
                    raise ValueError('Worker snapshot exceeds verified bound')
                raw = first - 0x18
                descriptor = q(raw)
                cookie = q(first - 8)
                workers.update(raw=hex(raw), descriptor=hex(descriptor), cookie=cookie)
                if cookie != count:
                    raise ValueError('Worker array cookie disagrees with manager')
                workers['descriptor_hex'] = read(descriptor, 32).hex()
                workers['allocation_hex'] = read(raw, 0x18 + count * 0x110).hex()
                workers['first_stable'] = q(args.shellcore_base + 0x2160df8) == first
        except (OSError, ValueError) as error:
            core['np_workers_error'] = str(error)
        # Native recovery worker: completed includes failure; preserve both words.
        try:
            worker = q(args.base + 0x26c7580)
            recovery = dict(pointer=hex(worker))
            result['recovery'] = recovery
            if worker:
                status, error = struct.unpack('<II', read(worker + 0x30, 8))
                recovery.update(status=status, error=hex(error),
                                root_stable=q(args.base + 0x26c7580) == worker)
        except (OSError, ValueError) as error:
            result['recovery_error'] = str(error)
        for i in range(8):
            sample = dict(time=time.time(), index=i)
            result['samples'].append(sample)
            try:
                raw = read(args.shellcore_base + 0x21667d0, 0x80)
                sample['sns_memory'] = dict(raw=raw.hex(), initialized=struct.unpack_from('<I', raw, 8)[0],
                                            size=hex(struct.unpack_from('<Q', raw, 0x10)[0]),
                                            backing=hex(struct.unpack_from('<Q', raw, 0x20)[0]),
                                            handle=hex(struct.unpack_from('<Q', raw, 0x28)[0]))
                callout = q(args.shellcore_base + 0x224c310)
                entry = dict(pointer=hex(callout))
                sample['trophy_callout'] = entry
                if callout:
                    raw = read(callout - 0x10, 0x48)
                    entry.update(raw=raw.hex(), descriptor=hex(struct.unpack_from('<Q', raw)[0]),
                                 vtable=hex(struct.unpack_from('<Q', raw, 0x10)[0]),
                                 thread=hex(struct.unpack_from('<Q', raw, 0x18)[0]),
                                 initialized=raw[0x40],
                                 root_stable=q(args.shellcore_base + 0x224c310) == callout)
                outer = q(args.shellcore_base + 0x224ad00)
                net = q(outer + 0x20) if outer else 0
                pool = q(net + 0x20) if net else 0
                entry = dict(outer=hex(outer), net_storage=hex(net), pointer=hex(pool))
                sample['trophy_http_pool'] = entry
                if pool:
                    raw = read(pool - 0x10, 0x50)
                    entry.update(raw=raw.hex(), descriptor=hex(struct.unpack_from('<Q', raw)[0]),
                                 vtable=hex(struct.unpack_from('<Q', raw, 0x10)[0]),
                                 capacity=struct.unpack_from('<i', raw, 0x20)[0],
                                 count=struct.unpack_from('<i', raw, 0x24)[0],
                                 mutex=hex(struct.unpack_from('<Q', raw, 0x40)[0]), initialized=raw[0x48],
                                 root_stable=q(args.shellcore_base + 0x224ad00) == outer and
                                             q(outer + 0x20) == net and q(net + 0x20) == pool)
            except (OSError, ValueError) as error:
                sample['np_lifecycle_error'] = str(error)
            try:
                queue = q(args.shellcore_base + 0x224c318)
                entry = dict(pointer=hex(queue))
                sample['trophy_queue'] = entry
                if queue:
                    raw = read(queue - 0x10, 0xe0)
                    word = lambda offset: struct.unpack_from('<Q', raw, 0x10 + offset)[0]
                    entry.update(descriptor=hex(struct.unpack_from('<Q', raw)[0]),
                                 vtable=hex(word(0)), stop=raw[0x18],
                                 mutex=hex(word(0x18)), condition=hex(word(0x30)),
                                 thread_vtable=hex(word(0x70)), thread=hex(word(0x78)),
                                 state=struct.unpack_from('<I', raw, 0xc8)[0],
                                 result=hex(struct.unpack_from('<I', raw, 0xcc)[0]),
                                 owner=hex(word(0xc0)), raw=raw.hex(),
                                 root_stable=q(args.shellcore_base + 0x224c318) == queue)
            except (OSError, ValueError) as error:
                sample['trophy_queue_error'] = str(error)
            try:
                owner = q(args.base + 0x13a8ee0)
                sample['owner'] = hex(owner)
                if not owner:
                    raise ValueError('Background owner is not initialized')
                renderer = q(owner + 8)
                sample['renderer'] = hex(renderer)
                if not renderer:
                    raise ValueError('Background renderer is not initialized')
                light = q(renderer + 0xc0)
                if q(light) != args.base + 0x1169f60:
                    raise ValueError('Unexpected Light vtable/module base')
                raw = read(renderer, 0xf4)
                sample.update(owner=hex(owner), renderer=hex(renderer), light=hex(light),
                              renderer_hex=raw.hex(), initialized=raw[0],
                              opacity=struct.unpack_from('<f', raw, 4)[0],
                              light_hex=read(light, 0xe0).hex())
                sample['vs'] = shader(q(light + 0x70))
                sample['ps'] = shader(q(light + 0x78))
                if i == 0:
                    try:
                        # Firmware 13.60 BGLayer AOT get_Instance and ImageMapper.
                        # Validate module identity before following the managed root.
                        identity = b'Sce.Vsh.ShellUI.BGLayer\0'
                        if read(args.bglayer_base + 0xa0090, len(identity)) != identity:
                            raise ValueError('Unexpected managed BGLayer module identity')
                        slot = q(args.bglayer_base + 0xcced8)
                        plugin = q(slot)
                        layer = q(plugin + 0x348)
                        managed = read(layer, 0x210)
                        word = lambda offset: struct.unpack_from('<I', managed, offset)[0]
                        sample['managed_background'] = dict(
                            slot=hex(slot), plugin=hex(plugin), layer=hex(layer), raw=managed.hex(),
                            theme_index=word(0xf0), current_state=word(0x1d8),
                            requested_state=word(0x1dc), native_theme_index=word(0x178),
                            light_flags=managed[0x1b3],
                            root_stable=q(args.bglayer_base + 0xcced8) == slot and
                                        q(slot) == plugin and q(plugin + 0x348) == layer)
                    except (OSError, ValueError) as error:
                        sample['managed_background_error'] = str(error)
                    try:
                        particle = q(renderer + 0xc8)
                        raw_particle = read(particle, 0x7f0)
                        if (struct.unpack_from('<Q', raw_particle)[0] != args.base + 0x1169d68 or
                                raw_particle[0x38:0x45] != b'LightParticle'):
                            raise ValueError('Unexpected LightParticle identity')
                        sample['particle'] = dict(pointer=hex(particle), raw=raw_particle.hex(),
                                                  opacity=struct.unpack_from('<f', raw_particle, 0x58)[0],
                                                  activation=raw_particle[0x720],
                                                  state=struct.unpack_from('<I', raw_particle, 0x7d4)[0],
                                                  root_stable=q(renderer + 0xc8) == particle)
                    except (OSError, ValueError) as error:
                        sample['particle_error'] = str(error)
                    try:
                        sample['transition'] = transition(renderer)
                    except (OSError, ValueError) as error:
                        sample['transition_error'] = str(error)
                graphics = q(args.base + 0x2d1f498)
                array = q(graphics + 0xe0)
                index = int.from_bytes(read(graphics + 0x100, 4), 'little')
                if index >= 64:
                    raise ValueError('Context selector exceeds bound')
                context = q(array + 8 * index)
                sample.update(context=hex(context), draw_serial=q(context + 0x88),
                              dcb_hex=read(context + 0x1c8, 0x38).hex(),
                              scissor=struct.unpack('<4i', read(context + 0x740, 16)),
                              selection_stable=q(array + 8 * index) == context and
                              int.from_bytes(read(graphics + 0x100, 4), 'little') == index)
                if i == 0:
                    bound = dict(blend_hex=read(context + 0x824, 8).hex(),
                                 viewport_hex=read(context + 0x750, 24).hex())
                    sample['bound'] = bound
                    for name, offset in [('ps', 0x5c0), ('vs', 0x5c8)]:
                        try:
                            bound[name] = shader(q(context + offset))
                        except (OSError, ValueError) as error:
                            bound[name] = dict(error=str(error))
                    for name, offset, descriptor_offset, length in [('ps_cb0', 0x3a8, 0x58, 6),
                                                                    ('vs_cb0', 0x3e8, 0x58, 6),
                                                                    ('ps_tex0', 0x248, 0x58, 32),
                                                                    ('rt0', 0x6f0, 0x19c, 128)]:
                        obj = q(context + offset)
                        entry = dict(object=hex(obj))
                        bound[name] = entry
                        if obj:
                            try:
                                desc = read(obj + descriptor_offset, length)
                                entry['descriptor_hex'] = desc.hex()
                                if length == 6:
                                    payload = int.from_bytes(desc, 'little')
                                    entry['payload'] = hex(payload)
                                    entry['prefix_hex'] = read(payload, 64).hex()
                            except (OSError, ValueError) as error:
                                entry['error'] = str(error)
                    sample['bound_context_stable'] = q(array + 8 * index) == context
            except (OSError, ValueError) as error:
                sample['error'] = str(error)
            if i < 7:
                time.sleep(0.125)
    finally:
        kernel.CloseHandle(process)
    result['read_bytes'] = bytes_read
    result['image'] = image.value
    output = Path(os.environ['LOCALAPPDATA']) / 'Temp/opencode' / f'background-{args.pid}-{time.time_ns()}.json'
    with output.open('x', encoding='utf-8') as file:
        json.dump(result, file, indent=2)
    print(output)
    print(json.dumps(result['samples'][-1], indent=2))


if __name__ == '__main__':
    main()
