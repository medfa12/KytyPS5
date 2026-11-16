"""Read-only experiments on hash-pinned Home HBC, not a guest/runtime test.

Requires an existing hermes-dec checkout (--decoder points at its src directory).
Executes only reviewed instruction subsets; React/native calls are explicit test
doubles. Unsupported instructions fail closed. Never loads or writes a database.
"""
import argparse
import hashlib
from io import BytesIO
import json
from pathlib import Path
import sys


SHA256 = '565693cb03f5aacf152c9e7b03b6dfb5ce92a6200c1ae303f84e75159104d87f'
UNDEFINED = object()


def truth(value):
    # JavaScript arrays/objects, including empty ones, are truthy.
    return value is not UNDEFINED and value is not None and value is not False and not (
        isinstance(value, (int, float, str)) and value in (0, '')
    )


def equal(a, b):
    if isinstance(a, (dict, list)) or isinstance(b, (dict, list)):
        return a is b
    return type(a) is type(b) and a == b


class Rejection(Exception):
    """Injected JS/native failure; only this exception enters HBC catch handlers."""


class Pending:
    def __init__(self, name):
        self.name = name


class Await:
    def __init__(self, value):
        self.value = value


def prop(obj, key):
    if isinstance(obj, dict):
        return obj.get(key, UNDEFINED)
    if isinstance(obj, list):
        if key == 'length':
            return len(obj)
        if key == 'every':
            return lambda callback: all(truth(callback(x)) for x in obj)
        if key == 'forEach':
            def for_each(callback):
                for value in obj:
                    callback(value)
                return UNDEFINED
            return for_each
        if key == 'fill':
            def fill(value):
                obj[:] = [value] * len(obj)
                return obj
            return fill
        if key == 'reduce':
            def reduce(callback, result):
                for i, value in enumerate(obj):
                    result = callback(result, value, i)
                return result
            return reduce
        if isinstance(key, int):
            return obj[key]
    raise NotImplementedError(f'Unmodeled property {key!r}')


class Probe:
    def __init__(self, reader, parse, unpack):
        self.reader, self.parse, self.unpack = reader, parse, unpack
        self.visited = set()

    def frame(self, fid, env=(), args=()):
        return Frame(self, fid, env, args)

    def call(self, fid, env=(), args=()):
        frame = self.frame(fid, env, args)
        state, value = frame.run()
        if state != 'complete':
            raise AssertionError(f'Unexpected {state} in synchronous function {fid}')
        return value


class Frame:
    def __init__(self, probe, fid, env, args):
        self.probe, self.fid = probe, fid
        probe.visited.add(fid)
        self.code = {i.original_pos: i for i in probe.parse(probe.reader.function_headers[fid], probe.reader)}
        self.env, self.args = list(env), (UNDEFINED, *args)
        self.r, self.pc, self.local = {}, 0, None
        self.saved, self.complete = None, False
        self.resume, self.error, self.caught = UNDEFINED, None, None
        self.trace = []

    def run(self, value=UNDEFINED, error=None, stop=None):
        self.resume, self.error = value, error
        r = self.r
        for _ in range(10000):
            if self.pc == stop:
                return 'boundary', UNDEFINED
            ins = self.code[self.pc]
            at, op = self.pc, ins.inst.name
            a = [getattr(ins, f'arg{i + 1}') for i in range(len(ins.inst.operands))]
            self.trace.append(at)
            self.pc = ins.next_pos
            try:
                if op in ('StartGenerator',):
                    pass
                elif op == 'ResumeGenerator':
                    if self.error is not None:
                        error, self.error = self.error, None
                        raise error
                    r[a[0]], r[a[1]] = self.resume, False
                elif op == 'SaveGenerator':
                    self.saved = at + a[0]
                elif op == 'CompleteGenerator':
                    self.complete = True
                elif op == 'Catch':
                    r[a[0]] = self.caught
                elif op == 'Ret':
                    if self.saved is not None and not self.complete:
                        self.pc, self.saved = self.saved, None
                        return 'yield', r[a[0]]
                    return 'complete', r[a[0]]
                elif op == 'LoadParam':
                    r[a[0]] = self.args[a[1]]
                elif op == 'GetEnvironment':
                    r[a[0]] = self.env[a[1]]
                elif op == 'CreateEnvironment':
                    self.local = r[a[0]] = {}
                elif op == 'LoadFromEnvironment':
                    r[a[0]] = r[a[1]][a[2]]
                elif op in ('StoreToEnvironment', 'StoreNPToEnvironment'):
                    r[a[0]][a[1]] = r[a[2]]
                elif op == 'CreateClosure':
                    env = [r[a[1]], *self.env]
                    r[a[0]] = lambda *args, fid=a[2], env=env: self.probe.call(fid, env, args)
                elif op == 'GetGlobalObject':
                    r[a[0]] = {'Array': lambda length: [UNDEFINED] * length,
                               'Object': {'entries': lambda obj: [list(pair) for pair in obj.items()]}}
                elif op == 'IteratorBegin':
                    if not isinstance(r[a[1]], list):
                        raise NotImplementedError('Only array iteration is modeled')
                    r[a[0]] = iter(r[a[1]])
                elif op == 'IteratorNext':
                    try:
                        r[a[0]] = next(r[a[1]])
                    except StopIteration:
                        r[a[0]] = r[a[1]] = UNDEFINED
                elif op == 'IteratorClose':
                    # Array iterator has no user return/close callback.
                    pass
                elif op in ('LoadConstUndefined', 'LoadConstNull', 'LoadConstTrue', 'LoadConstFalse', 'LoadConstZero'):
                    r[a[0]] = {'LoadConstUndefined': UNDEFINED, 'LoadConstNull': None,
                               'LoadConstTrue': True, 'LoadConstFalse': False, 'LoadConstZero': 0}[op]
                elif op in ('LoadConstUInt8', 'LoadConstInt'):
                    r[a[0]] = a[1]
                elif op == 'LoadConstString':
                    r[a[0]] = self.probe.reader.strings[a[1]]
                elif op == 'Mov':
                    r[a[0]] = r[a[1]]
                elif op in ('GetById', 'GetByIdShort', 'TryGetById'):
                    r[a[0]] = prop(r[a[1]], self.probe.reader.strings[a[3]])
                elif op == 'GetByVal':
                    r[a[0]] = prop(r[a[1]], r[a[2]])
                elif op in ('PutNewOwnById', 'PutNewOwnByIdShort'):
                    r[a[0]][self.probe.reader.strings[a[2]]] = r[a[1]]
                elif op == 'NewObject':
                    r[a[0]] = {}
                elif op == 'NewArray':
                    # Operand is capacity, not JS length.
                    r[a[0]] = []
                elif op == 'NewArrayWithBuffer':
                    values = self.probe.unpack(self.probe.reader.arrays[a[3]:], a[2], 89)
                    r[a[0]] = [json.loads(v) for v in values.to_strings(self.probe.reader.strings)]
                elif op == 'PutOwnByIndex':
                    while len(r[a[0]]) <= a[2]:
                        r[a[0]].append(UNDEFINED)
                    r[a[0]][a[2]] = r[a[1]]
                elif op in ('Call1', 'Call2', 'Call3', 'Call4'):
                    # Test doubles use bound Python callables; thisArg is ignored.
                    r[a[0]] = r[a[1]](*(r[x] for x in a[3:]))
                elif op in ('StrictEq', 'StrictNeq'):
                    result = equal(r[a[1]], r[a[2]])
                    r[a[0]] = result if op == 'StrictEq' else not result
                elif op == 'Not':
                    r[a[0]] = not truth(r[a[1]])
                elif op == 'TypeOf':
                    v = r[a[1]]
                    r[a[0]] = 'function' if callable(v) else 'string' if isinstance(v, str) else 'object'
                elif op in ('Sub', 'Div'):
                    r[a[0]] = r[a[1]] - r[a[2]] if op == 'Sub' else r[a[1]] / r[a[2]]
                elif op in ('Jmp', 'JmpLong'):
                    self.pc = at + a[0]
                elif op in ('JmpFalse', 'JmpFalseLong', 'JmpTrue', 'JmpTrueLong'):
                    if truth(r[a[1]]) == op.startswith('JmpTrue'):
                        self.pc = at + a[0]
                elif op in ('JStrictEqual', 'JStrictNotEqual'):
                    if equal(r[a[1]], r[a[2]]) == (op == 'JStrictEqual'):
                        self.pc = at + a[0]
                else:
                    raise NotImplementedError(f'Unmodeled opcode {op} in #{self.fid}+{at:x}')
            except Rejection as exc:
                handlers = self.probe.reader.function_id_to_exc_handlers.get(self.fid, [])
                handler = next((h for h in handlers if h.start <= at < h.end), None)
                if handler is None:
                    raise
                self.pc, self.caught = handler.target, exc
        raise AssertionError('Instruction budget exceeded')


def drive(frame):
    """Drive reviewed generator bytecode with explicit await outcomes, no clock."""
    outputs, value, error = [], UNDEFINED, None
    for _ in range(100):
        state, value = frame.run(value, error)
        error = None
        if state == 'complete':
            return {'state': state, 'outputs': outputs}
        if isinstance(value, Await):
            value = value.value
        elif value is not UNDEFINED and not isinstance(value, Pending):
            outputs.append(value)
            value = UNDEFINED
        if isinstance(value, Pending):
            return {'state': 'pending', 'request': value.name, 'outputs': outputs}
        if isinstance(value, Rejection):
            error, value = value, UNDEFINED
    raise AssertionError('Yield budget exceeded')


def experiments(p):
    results = {}
    # Root's config atom is an earlier gate than the experience/startup gates.
    # JSX only constructs explicit test nodes here: no React rendering, effects,
    # promise settlement or native module implementation is simulated.
    for name, config in (('undefined', UNDEFINED), ('null', None),
                         ('configured', {'isEnabled': lambda key: False})):
        nodes = []
        def jsx(component, props):
            node = {'component': component, 'props': props}
            nodes.append(node)
            return node
        env = {0: {'default': lambda values, count: values[:count]},
               1: {'AppConfig': 'AppConfig'},
               4: {'useAtom': lambda atom: [config, 'onConfigured']},
               5: {'useEffect': lambda effect, deps: None},
               6: {'default': 'configAtom'}, 7: {'default': 'RootWrapper'},
               8: {'default': 'configDefinition'},
               9: {'useSendAppStateToNotification': lambda: None},
               10: {'default': 'MainContent'}, 11: {'jsx': jsx},
               12: ['line', 'minimumVshVersion', 'devkitType', 'countryCode', 'minimumAge', 'isPlusMember']}
        root = p.call(3081, [env])
        present = name == 'configured'
        assert root['component'] == 'AppConfig'
        assert root['props']['appConfig'] == 'configDefinition'
        assert root['props']['onConfigured'] == 'onConfigured'
        assert root['props']['verifiers'] == env[12]
        assert [node['component'] for node in nodes] == (
            ['MainContent', 'RootWrapper', 'AppConfig'] if present else ['AppConfig'])
        assert truth(root['props']['children']) is present
        results['root_config_' + name] = dict(content_node_constructed=present,
                                              jsx_nodes=[node['component'] for node in nodes])
    for name, revision, items in [
        ('revision_rejected_empty', Rejection('revision failed'), []),
        ('revision_rejected_items', Rejection('revision failed'), [{'spaceId': 'game'}]),
        ('revision_pending', Pending('revision'), []),
        ('items_pending', 'revision-key', Pending('items')),
        ('items_rejected', 'revision-key', Rejection('items failed')),
    ]:
        calls = []
        def read(options):
            calls.append(options)
            return items
        cache = {'get': lambda key: UNDEFINED, 'set': lambda key, value: UNDEFINED}
        options = {'cache': cache, 'key': lambda: revision, 'read': read, 'hash': UNDEFINED}
        env = [{}, {}, {0: {'default': Await}, 2: lambda value: 0}]
        frame = p.frame(8786, env, (options,))
        try:
            result = drive(frame)
        except Rejection as exc:
            result = {'state': 'rejected', 'error': str(exc)}
        result['read_calls'] = calls
        result['revision_catch_taken'] = 0x104 in frame.trace
        results[name] = result
    assert results['revision_rejected_empty']['outputs'] == [[[], 0]]
    assert results['revision_rejected_items']['outputs'] == [[[{'spaceId': 'game'}], 0]]
    for name in ('revision_rejected_empty', 'revision_rejected_items'):
        assert results[name]['state'] == 'complete'
        assert results[name]['revision_catch_taken']
        assert results[name]['read_calls'] == [{'isCached': False}]
    assert results['revision_pending']['state'] == 'pending'
    assert results['revision_pending']['read_calls'] == []
    assert results['items_pending']['request'] == 'items'
    assert results['items_pending']['read_calls'] == [{'isCached': False}]
    assert results['items_rejected']['state'] == 'rejected'

    # Execute actual startup initializer prefix through construction of values.
    module = {8: {'useAtomValue': lambda atom: atom}, 11: {'default': 'login'},
              12: {'default': 3}, 10: {'default': []},
              6: {'useAnimatedValue': lambda value: value, 'useAnimatedValues': lambda init: init()},
              9: {'useMemo': lambda fn, deps: fn()}}
    init = p.frame(9008, [module], (lambda: None,))
    assert init.run(stop=0x104)[0] == 'boundary'
    initial = init.r[1]
    assert initial == dict(experienceScales=[1, 1, 1], hub=1, switcher=1,
                           system=1, systemOpacity=1, titleOpacity=1)
    results['initial_values'] = initial
    # Actual style-building prefix, with numeric interpolation doubles. Stop
    # before copyDataProperties/StyleSheet; all four opacity fields exist here.
    for value in (1, 0):
        def animated():
            def interpolate(spec):
                lo, hi = spec['inputRange']
                a, b = spec['outputRange']
                return a + (b - a) * (value - lo) / (hi - lo)
            return {'interpolate': interpolate}
        values = {key: animated() for key in ('hub', 'switcher', 'system', 'systemOpacity', 'titleOpacity')}
        values['experienceScales'] = []
        style = p.frame(9040, [{0: values}, {}, {6: {'SCALED_EXP_SIZE': 100, 'EXPERIENCE_SIZE': 80}}])
        assert style.run(stop=0x1ef)[0] == 'boundary'
        opacity = {key: style.r[3][key]['opacity'] for key in ('strandContainer', 'titleContainer', 'system', 'hubViewer')}
        assert set(opacity.values()) == {1 - value}
        results[f'opacity_at_{value}'] = opacity

    for count, released, mounted in ((0, False, True), (1, False, True), (1, True, True), (1, True, False)):
        calls = []
        env = {2: [{}] * count, 1: 3, 10: lambda: calls.append('reset'),
               11: lambda opts: calls.append('start'), 13: lambda opts: calls.append('prepare'),
               14: lambda: None, 16: UNDEFINED if released else Pending('local startup signal'),
               17: {'current': mounted}}
        frame = p.frame(9016, [{}, env, {14: {'SPRING_OPTIONS_SLOW': {}}}])
        result = drive(frame)
        result['calls'] = calls
        results[f'startup_{count}_{released}_{mounted}'] = result
        assert ('start' in calls) == (count > 0 and released and mounted)
        assert (result['state'] == 'pending') == (count > 0 and not released)
        if count == 0:
            assert calls == []

    for count in (0, 1):
        for phase in ('login', 'hidden', 'finished'):
            calls = []
            env = {18: bool(count), 12: phase, 0: 'other',
                   19: lambda: calls.append('startup'), 10: lambda: calls.append('reset'),
                   9: lambda: calls.append('forceFinish')}
            p.call(9019, [env])
            assert calls == ([] if not count else [{'login': 'startup', 'hidden': 'reset', 'finished': 'forceFinish'}[phase]])
            results[f'effect_{count}_{phase}'] = calls

    # Actual timing-event dispatcher: firstPaint/widgetReady cannot stand in
    # for interactive when the registered release callback has that name.
    for name, capture in (('interactive', True), ('firstPaint', True),
                          ('widgetReady', True), ('interactive', False)):
        released = []
        env = [{0: capture, 1: UNDEFINED, 2: {'interactive': lambda stamp: released.append(stamp)}}]
        event = {'nativeEvent': {'name': name, 'startTime': 123}}
        p.call(1958, env, (event,))
        assert released == ([123] if name == 'interactive' and capture else [])
        results[f'timing_{name}_{capture}'] = released

    # Module 61 selects the current space, not the union of both lists.
    by_space = {'game': [], 'media': [{'spaceId': 'media'}]}
    env = [{0: {'default': lambda space: by_space[space]}, 1: {'default': 'game'}}]
    assert p.call(464, env, (lambda atom: atom,)) == []
    results['media_only_selected_game'] = []
    assert p.call(404, [{0: {'atom': lambda initial: initial}}], ('game',)) == []
    for missing in (False, True):
        atoms = {i: {'default': None if missing and i == 2 else (lambda: None)} for i in range(4)}
        ready = p.call(5730, [atoms], (lambda atom: atom,))
        assert ready is (not missing)
        results[f'callback_holder_missing_{missing}'] = ready
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    parser.add_argument('--decoder', type=Path, required=True)
    parser.add_argument('--dump', type=int, nargs='+', help='Disassemble selected function IDs instead of probing')
    args = parser.parse_args()
    data = args.bundle.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != SHA256:
        parser.error('Bundle hash does not match reviewed regular 13.60 Home')
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(args.decoder.resolve()))
    from hermes_dec.parsers.hbc_file_parser import HBCReader
    from hermes_dec.parsers.hbc_bytecode_parser import parse_hbc_bytecode
    from hermes_dec.parsers.serialized_literal_parser import unpack_slp_array
    reader = HBCReader()
    reader.read_whole_file(BytesIO(data))
    assert reader.header.version == 89
    if args.dump:
        print('SHA256', digest)
        for fid in args.dump:
            header = reader.function_headers[fid]
            print(f'\nFunction #{fid} file+0x{header.offset:x} size=0x{header.bytecodeSizeInBytes:x}')
            for handler in reader.function_id_to_exc_handlers.get(fid, []):
                print(f'catch [0x{handler.start:x},0x{handler.end:x}) -> 0x{handler.target:x}')
            for instruction in parse_hbc_bytecode(header, reader):
                print(instruction)
        return
    probe = Probe(reader, parse_hbc_bytecode, unpack_slp_array)
    results = experiments(probe)
    assert hashlib.sha256(args.bundle.read_bytes()).hexdigest() == digest
    print(json.dumps({'bundle_sha256': digest, 'scope': 'offline bytecode subset with test doubles',
                      'functions_executed': sorted(probe.visited), 'cases': results,
                      'live_psn37_gate': 'unproven', 'input_unchanged': True}, indent=2))


if __name__ == '__main__':
    main()
