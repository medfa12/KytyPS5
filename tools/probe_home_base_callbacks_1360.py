"""Execute two hash-pinned RNPS base HBC functions with explicit JS test doubles.

This bounded subset is not Hermes, a guest trace, or a promise-runtime test.
It checks the actual bytecode's callback ID packing/map selection/consumption.
"""
import argparse
import hashlib
from io import BytesIO
from pathlib import Path
import sys


SHA256 = '12c7a876da06d1d01dc9c8dd559c6dd41c961f187863d637d6003e1e72dffd74'
UNDEFINED = object()


class Map:
    def __init__(self): self.values = {}
    def get(self, key): return self.values.get(key, UNDEFINED)
    def set(self, key, value): self.values[key] = value
    def delete(self, key): return self.values.pop(key, UNDEFINED) is not UNDEFINED


def prop(value, name):
    if isinstance(value, Map) and name in ('get', 'set', 'delete'): return getattr(value, name)
    if isinstance(value, list) and name == 'push': return lambda item: value.append(item)
    if isinstance(value, dict): return value.get(name, UNDEFINED)
    raise ValueError(f'unsupported property {name!r}')


def truth(value):
    return value is not UNDEFINED and value is not None and value is not False and value != 0 and value != ''


def run(reader, parse, fid, args):
    code = {x.original_pos: x for x in parse(reader.function_headers[fid], reader)}
    regs, pc = {}, 0
    for _ in range(1000):
        ins = code[pc]
        at, op = pc, ins.inst.name
        a = [getattr(ins, f'arg{i + 1}') for i in range(len(ins.inst.operands))]
        pc = ins.next_pos
        if op == 'LoadParam': regs[a[0]] = args[a[1]]
        elif op == 'Mov': regs[a[0]] = regs[a[1]]
        elif op == 'GetGlobalObject': regs[a[0]] = {'Date': {'now': lambda: 123}}
        elif op in ('GetById', 'GetByIdShort', 'TryGetById'): regs[a[0]] = prop(regs[a[1]], reader.strings[a[3]])
        elif op == 'PutById': regs[a[0]][reader.strings[a[3]]] = regs[a[1]]
        elif op in ('LoadConstUInt8', 'LoadConstInt'): regs[a[0]] = a[1]
        elif op == 'LoadConstZero': regs[a[0]] = 0
        elif op == 'LoadConstUndefined': regs[a[0]] = UNDEFINED
        elif op == 'NewArray': regs[a[0]] = []
        elif op == 'LShift': regs[a[0]] = regs[a[1]] << regs[a[2]]
        elif op == 'URshift': regs[a[0]] = (regs[a[1]] & 0xffffffff) >> regs[a[2]]
        elif op == 'BitAnd': regs[a[0]] = regs[a[1]] & regs[a[2]]
        elif op == 'BitOr': regs[a[0]] = regs[a[1]] | regs[a[2]]
        elif op == 'Inc': regs[a[0]] = regs[a[1]] + 1
        elif op in ('Call1', 'Call2', 'Call3'): regs[a[0]] = regs[a[1]](*(regs[x] for x in a[3:]))
        elif op == 'CallBuiltin' and a[1] == 46:
            # This pinned function supplies target/source/index in r7/r6/r5.
            if a[2] != 4 or not isinstance(regs[7], list) or not isinstance(regs[6], list) or regs[5] != 0:
                raise ValueError('unexpected arraySpread register contract')
            regs[7].extend(regs[6]); regs[a[0]] = len(regs[7])
        elif op == 'CallBuiltin' and a[1] == 47:
            if a[2] != 4 or not callable(regs[7]) or not isinstance(regs[6], list) or regs[5] is not UNDEFINED:
                raise ValueError('unexpected apply register contract')
            regs[a[0]] = regs[7](*regs[6])
        elif op == 'Jmp': pc = at + a[0]
        elif op in ('JmpTrue', 'JmpFalse'):
            if truth(regs[a[1]]) == (op == 'JmpTrue'): pc = at + a[0]
        elif op == 'Ret': return regs[a[0]]
        else: raise ValueError(f'unmodeled opcode {op} in function {fid} at {at:#x}')
    raise ValueError('instruction budget exhausted')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('bundle', type=Path)
    p.add_argument('--decoder', type=Path, required=True)
    args = p.parse_args()
    data = args.bundle.read_bytes()
    if hashlib.sha256(data).hexdigest() != SHA256: raise ValueError('base HBC hash mismatch')
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(args.decoder.resolve()))
    from hermes_dec.parsers.hbc_file_parser import HBCReader
    from hermes_dec.parsers.hbc_bytecode_parser import parse_hbc_bytecode
    reader = HBCReader(); reader.read_whole_file(BytesIO(data))
    if reader.header.version != 89: raise ValueError('wrong HBC version')
    def scenario(reverse=False, use_reject=False):
        received = []
        resolve = lambda *values: received.append(('resolve', values))
        reject = lambda *values: received.append(('reject', values))
        state = {'_callID': 90, '_successCallbacks': Map(), '_failureCallbacks': Map()}
        params = []
        # Function2520's Promise closure supplies resolve first, reject second.
        callbacks = (reject, resolve) if reverse else (resolve, reject)
        run(reader, parse_hbc_bytecode, 363, [state, 0, 0, params, *callbacks])
        if params != [180, 181]: raise AssertionError('actual callback IDs differ')
        callback_id = params[1] if use_reject else params[0]
        run(reader, parse_hbc_bytecode, 371, [state, callback_id, ['payload']])
        # A second invocation must not dispatch after both maps were consumed.
        run(reader, parse_hbc_bytecode, 371, [state, callback_id, ['again']])
        return received
    assert scenario() == [('resolve', ('payload',))]
    assert scenario(use_reject=True) == [('reject', ('payload',))]
    assert scenario(reverse=True) == [('reject', ('payload',))]
    if hashlib.sha256(args.bundle.read_bytes()).hexdigest() != SHA256: raise RuntimeError('HBC source changed')
    print('Base HBC 363/371: even resolves, odd rejects, callbacks consumed once; reversed-input negative control rejects.')
    print('Static HBC subset with test doubles; does not establish live JS delivery.')


if __name__ == '__main__':
    main()
