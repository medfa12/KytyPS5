"""Pure read/decoding/correlation helpers for capture_home_gates_1360.py.

No process APIs here. Unknown layouts/identities are errors, never empty results.
Only allowlisted strings leave the decoder; item contents are never traversed.
"""
import hashlib
import re
import struct
from pathlib import Path


IMAGES = {
    'vsh': ('ReactNative.Modules.Vsh.dll.sprx', '64b35bdf80037c98dd4e1ffa061a4c19fec82e2f90398618bb9471955cdcc9ee'),
    'pui': ('ReactNative.PUI.dll.sprx', 'a9fe48498b4ffe327c5fa33b6bffd7e6349eec40974677f101c09b2913616914'),
    'json': ('Newtonsoft.Json.PlayStation.dll.sprx', 'bac49935d17da7be951503c44b5a213d22ac835e67c3ec673375d373a56a03aa'),
    'mono': ('libmonosgen-2.0.sprx', '294af118b930006345855ecc63e452d3fa28a2c138136755c6f2066b15938a40'),
    'corlib': ('mscorlib.dll.sprx', 'c05d3ebb585d8185530793eb259200337fe20b8d656df20cbcd7437437d48ce3'),
}
HOME_HASH = '565693cb03f5aacf152c9e7b03b6dfb5ce92a6200c1ae303f84e75159104d87f'
PROBES = {'execute': ('vsh', 0x471f0), 'resolve': ('pui', 0x1cba30),
          'reject': ('pui', 0x1cbaa0), 'dispatch': ('pui', 0x555a0),
          'predicate': ('pui', 0x177cc6)}
CALLERS = {0x473d5: 'condition', 0x475b6: 'scalar2',
           0x476b9: 'items', 0x477a6: 'items2'}
# Audited Execute callers whose argument contracts are deliberately not decoded.
EXCLUDED_CALLERS = {0x474c6: 'GetScalar', 0x47896: 'GetIndexById',
                    0x479b1: 'GetItemById', 0x47a96: 'Insert',
                    0x47b86: 'Update', 0x47c76: 'Remove'}
# Relocation-free executable windows; all are inside full-hash-pinned inputs.
WINDOWS = {
    'vsh': [(0x471f0, 16), (0x473cb, 10), (0x475ac, 10), (0x476af, 10),
            (0x4779c, 10), (0x48720, 32), (0x487c0, 32)],
    'pui': [(0x1cba30, 16), (0x1cbaa0, 16), (0x555a0, 16),
            (0x555b8, 7), (0x556ef, 16), (0x87700, 8),
            (0x92800, 10), (0x8f180, 13), (0x1150, 10),
            (0x177caa, 36), (0x88810, 10), (0x8975b, 27),
            (0x92510, 21), (0x876f0, 16), (0x16e7dc, 27), (0x16968c, 11)],
    'json': [(0x2ca30, 10), (0x2ede0, 10), (0x30100, 30), (0x38930, 10)],
    'mono': [(0x42dae, 20), (0x42e61, 159)],
    'corlib': [(0x27b525, 12), (0x27cc70, 34)],
}
COLUMNS = ('bgMatteType', 'conceptId', 'conceptInfo', 'deeplinkUri', 'hubAppUri',
           'icon0PathWithTs', 'localConceptId', 'pic0PathWithTs', 'pinnedStatus',
           'sndPathWithTs', 'titleId', 'titleName')
KEYS = {'userId', 'contentLimit', 'filterTypeDispLocationExperienceSwitcher',
        'filterTypeDispLocationSpace', 'func', 'itemProps', 'offset', 'limit',
        'scalarProps', 'scalarMap', 'ctxId', 'items', 'revision'}
WORDS = set(COLUMNS) | {'game', 'media', 'display', 'GetItems', 'revision',
                        'interactive', 'firstPaint', 'widgetReady', 'rnps-home'}
TYPE_NAMES = frozenset({
    'System.String', 'System.Int32', 'System.Int64', 'System.WeakReference',
    'System.WeakReference`1', 'System.Collections.Generic.List`1',
    'ReactNative.Bridge.ReactInstance', 'ReactNative.Bridge.ReactContextImp',
    'ReactNative.PUI.PUIReactContext', 'ReactNative.PUI.PUIReactApp',
    'ReactNative.PUI.AppData', 'ReactNative.ReactApplicationManifest',
    'ReactNative.UIManager.ThemedReactContext',
    'ReactNative.UIManager.Events.ReactPerformanceTimingEvent',
    'ReactNative.UIManager.Events.RCTEventEmitter',
    'ReactNative.UIManager.Events.Event', 'ReactNative.UIManager.Events.EventDispatcher',
    'ReactNative.Bridge.JavaScriptModuleBase',
    'ReactNative.Modules.Core.RCTDeviceEventEmitter',
    'ReactNative.Modules.Core.RCTNativeAppEventEmitter',
    'ReactNative.Modules.Vsh.Database.DatabaseModule',
    'Newtonsoft.Json.Linq.JArray', 'Newtonsoft.Json.Linq.JObject',
    'Newtonsoft.Json.Linq.JPropertyKeyedCollection',
    'Newtonsoft.Json.Linq.JProperty', 'Newtonsoft.Json.Linq.JValue',
})


def diagnostic_type(kind):
    return kind if kind in TYPE_NAMES else 'other'


class DecodeError(Exception):
    """Message is a fixed diagnostic code, never target data."""
    def __init__(self, code, **details):
        super().__init__(code)
        self.details = details


class ScopeExcluded(DecodeError):
    """Known scope filter, not an unsupported object layout or Home association."""


def require(ok, code):
    if not ok:
        raise DecodeError(code)


def digest(data):
    return hashlib.sha256(data).hexdigest()


class Image:
    def __init__(self, data, expected):
        require(digest(data) == expected, 'image_hash')
        require(data[:6] == b'\x7fELF\x02\x01', 'elf_format')
        off = struct.unpack_from('<Q', data, 32)[0]
        size, count = struct.unpack_from('<HH', data, 54)
        require(size == 56 and 0 < count < 128 and off + size * count <= len(data), 'elf_headers')
        self.data = data
        self.loads = [s for i in range(count)
                      if (s := struct.unpack_from('<IIQQQQQQ', data, off + i * size))[0] == 1]

    def at(self, rva, size):
        segs = [s for s in self.loads if s[3] <= rva and rva + size <= s[3] + s[5]]
        require(len(segs) == 1 and segs[0][1] & 1, 'rva_not_executable')
        s = segs[0]
        off = s[2] + rva - s[3]
        require(off + size <= len(self.data), 'rva_file_bounds')
        return self.data[off:off + size]


def provenance(original, stage, bundle, log):
    require(digest(bundle.read_bytes()) == HOME_HASH, 'home_hash')
    images, bases, report = {}, {}, {}
    text = log.read_text(encoding='utf-8', errors='replace')
    for key, (name, sha) in IMAGES.items():
        data = (original / name).read_bytes()
        image = Image(data, sha)
        require((stage / name).read_bytes() == data, 'stage_diff_' + key)
        if key == 'corlib':
            # Layout reference only. psn37 has two corlib instances; no address
            # or trap is selected from either. List ABI is audited in this image.
            for rva, size in WINDOWS[key]:
                image.at(rva, size)
            images[key] = image
            report[key] = dict(name=name, sha256=sha, role='offline_layout_reference')
            continue
        placements = re.findall(r'place ' + re.escape(name) +
                                r' base=(0x[\da-fA-F]+) mapped=(0x[\da-fA-F]+)', text)
        require(len(placements) == 1, 'ambiguous_mapping_' + key)
        base, extent = (int(x, 16) for x in placements[0])
        require(base > 0 and base % 4096 == 0, 'mapping_alignment')
        require(all(s[3] + s[6] <= extent for s in image.loads), 'mapping_extent')
        loads = re.findall(r'^Loading: (.*[\\/]' + re.escape(name) + r')\s*$', text, re.M)
        require(bool(loads) and all(Path(p.strip()).resolve() == (stage / name).resolve()
                                   for p in loads), 'stage_log_path_' + key)
        for rva, size in WINDOWS[key]:
            image.at(rva, size)
        images[key], bases[key] = image, base
        report[key] = dict(name=name, sha256=sha, base=hex(base), extent=extent,
                           original=str((original / name).resolve()), stage=str((stage / name).resolve()))
    ranges = sorted((bases[k], bases[k] + report[k]['extent']) for k in bases)
    require(all(a[1] <= b[0] for a, b in zip(ranges, ranges[1:])), 'mapping_overlap')
    # These direct calls establish the four Execute return-address discriminators.
    for ret in CALLERS.keys() | EXCLUDED_CALLERS.keys():
        raw = images['vsh'].at(ret - 5, 5)
        require(raw[0] == 0xe8 and ret + struct.unpack('<i', raw[1:])[0] == 0x471f0, 'execute_call_abi')
    require(images['pui'].at(0x177cc0, 10) == bytes.fromhex('ff90880100004c0fb6f8'), 'predicate_abi')
    return images, bases, report


class Reader:
    def __init__(self, read, mono_base, budget=65536):
        self.read, self.mono, self.remaining = read, mono_base, budget
        self.site, self.type_path = 'object', {}

    def diagnostic(self, error):
        return dict(site=self.site, type_path=dict(self.type_path), **error.details)

    def raw(self, p, n):
        require(0x10000 <= p < 0x800000000000 and 0 <= n <= 2048, 'read_bounds')
        self.remaining -= n
        require(self.remaining >= 0, 'read_budget')
        try:
            data = self.read(p, n)
        except (OSError, RuntimeError):
            raise DecodeError('unreadable') from None
        require(len(data) == n, 'short_read')
        return data

    def q(self, p, site=None):
        if site is not None:
            self.site = site
        return struct.unpack('<Q', self.raw(p, 8))[0]

    def i(self, p):
        return struct.unpack('<i', self.raw(p, 4))[0]

    def byte(self, p):
        return self.raw(p, 1)[0]

    def cstr(self, p):
        data = bytearray()
        for i in range(160):
            b = self.byte(p + i)
            if not b:
                return data.decode('ascii')
            require(32 <= b < 127, 'class_ascii')
            data.append(b)
        raise DecodeError('class_length')

    def kind(self, p):
        klass = self.q(self.q(p))
        return self.cstr(self.q(klass + 0x48)) + '.' + self.cstr(self.q(klass + 0x40))

    def typed(self, p, kind, site=None):
        if site is not None:
            self.site = site
        try:
            actual = self.kind(p)
        except DecodeError as error:
            raise DecodeError(str(error), expected_type=diagnostic_type(kind),
                              observed_type='null' if not p else 'unreadable') from None
        if site is not None and len(self.type_path) < 16:
            self.type_path[site] = diagnostic_type(actual)
        if actual != kind:
            raise DecodeError('object_type', expected_type=diagnostic_type(kind),
                              observed_type=diagnostic_type(actual))
        return p

    def string(self, p, allowed, site=None):
        self.typed(p, 'System.String', site)
        n = self.i(p + 0x10)
        require(0 <= n <= 128, 'string_bound')
        try:
            value = self.raw(p + 0x14, n * 2).decode('utf-16-le')
        except UnicodeError:
            raise DecodeError('string_encoding') from None
        return value if value in allowed else None

    def weak(self, p, generic=True, site=None):
        self.typed(p, 'System.WeakReference`1' if generic else 'System.WeakReference', site)
        handle = self.i(p + (0x10 if generic else 0x14)) & 0xffffffff
        kind, slot = (handle & 7) - 1, handle >> 3
        require(kind in (0, 1), 'not_weak_handle')
        table = self.mono + 0x4f98e0 + kind * 0x138
        require(slot < (self.i(table + 0x100) & 0xffffffff), 'weak_capacity')
        n = slot + 32
        power = n.bit_length() - 1
        entry = self.q(table + (power - 5) * 8) + (n - (1 << power)) * 8
        raw = self.q(entry)
        require(raw & 3 == 3, 'weak_collected')
        target = (raw ^ 0xffffffffffffffff) & 0xfffffffffffffffc
        require(self.q(entry) == raw and target != 0, 'weak_changed')
        return target

    def instance_context(self, instance, site='identity.instance'):
        self.typed(instance, 'ReactNative.Bridge.ReactInstance', site)
        return self.weak(self.q(instance + 0x10, site + '.weak'), site=site + '.weak')

    def identity(self, context):
        bridge = 0
        self.site = 'identity.context'
        kind = self.kind(context)
        if kind == 'ReactNative.UIManager.ThemedReactContext':
            context = self.q(context + 0x10)
            kind = self.kind(context)
        if kind == 'ReactNative.Bridge.ReactContextImp':
            bridge = context
            context = self.instance_context(self.q(context + 0x10))
        self.typed(context, 'ReactNative.PUI.PUIReactContext', 'identity.pui')
        # PUIReactContext+30 is its nested Data, NOT a manifest. The manifest
        # belongs to PUIReactApp (ApplicationName+8f180/+8f189 in pinned PUI).
        data = self.typed(self.q(context + 0x68, 'identity.app_data'),
                          'ReactNative.PUI.AppData', 'identity.app_data')
        app = self.typed(self.weak(self.q(data + 0x10, 'identity.app_weak'), False,
                                  'identity.app_weak'), 'ReactNative.PUI.PUIReactApp', 'identity.app')
        self.site = 'identity.app_backlinks'
        require(self.q(app + 0x38) == context and self.q(app + 0x10) == data, 'generation_backlink')
        manifest = self.q(app + 0x30, 'identity.manifest')
        self.typed(manifest, 'ReactNative.ReactApplicationManifest', 'identity.manifest')
        name = self.string(self.q(manifest + 0x30, 'identity.application_name'),
                           {'rnps-home'}, 'identity.application_name')
        if name != 'rnps-home':
            # A valid non-Home manifest is excluded even before a user is launched.
            # Do not expose arbitrary application names or attribute it to Login.
            raise ScopeExcluded('not_home')
        self.site = 'identity.launched_user'
        require(self.byte(context + 0x88) == 1, 'user_unavailable')
        user = self.i(context + 0x8c)
        uid = self.raw(app + 0x98, 16).hex()
        require(int(uid, 16) != 0, 'generation_uid_absent')
        return dict(generation=uid, user=user, pui_context=hex(context), bridge_context=hex(bridge))

    def list_info(self, p, limit=1000000):
        self.typed(p, 'System.Collections.Generic.List`1')
        count, array = self.i(p + 0x20), self.q(p + 0x10)
        require(0 <= count <= limit, 'list_count')
        require(self.q(array + 0x10) == 0, 'array_rank')
        capacity = self.q(array + 0x18)
        require(count <= capacity <= 1000000, 'list_capacity')
        return count, array

    def children(self, p, array=False):
        self.typed(p, 'Newtonsoft.Json.Linq.JArray' if array else 'Newtonsoft.Json.Linq.JObject')
        collection = self.q(p + 0x38)
        if not array:
            self.typed(collection, 'Newtonsoft.Json.Linq.JPropertyKeyedCollection')
            collection = self.q(collection + 0x10)
        return self.list_info(collection, 64 if not array else 1000000)

    def fields(self, p):
        count, array = self.children(p)
        result = {}
        for i in range(count):
            prop = self.typed(self.q(array + 0x20 + i * 8), 'Newtonsoft.Json.Linq.JProperty')
            name = self.string(self.q(prop + 0x40), KEYS)
            if name is not None:
                require(name not in result, 'duplicate_property')
                result[name] = self.q(self.q(prop + 0x38) + 0x10)
        return result

    def value(self, p):
        self.typed(p, 'Newtonsoft.Json.Linq.JValue')
        box = self.q(p + 0x30)
        if not box:
            return None
        kind = self.kind(box)
        if kind == 'System.String':
            return self.string(box, WORDS)
        if kind == 'System.Int32':
            return self.i(box + 0x10)
        if kind == 'System.Int64':
            return struct.unpack('<q', self.raw(box + 0x10, 8))[0]
        raise DecodeError('value_type')

    def columns(self, p):
        count, array = self.children(p, True)
        if count != len(COLUMNS):
            return False
        return tuple(self.value(self.q(array + 0x20 + i * 8)) for i in range(count)) == COLUMNS

    def condition(self, p):
        f = self.fields(p)
        names = ('userId', 'contentLimit', 'filterTypeDispLocationExperienceSwitcher',
                 'filterTypeDispLocationSpace')
        require(all(k in f for k in names), 'condition_incomplete')
        user, limit, display, space = (self.value(f[k]) for k in names)
        require(type(user) is int and type(limit) is int and display == 'display'
                and space in ('game', 'media'), 'condition_not_home_space')
        return dict(user=user, content_limit=limit, space=space)

    def item_count(self, result):
        f = self.fields(result)
        require('items' in f, 'items_missing')
        return self.children(f['items'], True)[0]


class Correlator:
    """One record per observed request, with immutable generation/condition epoch.

    A native Resolve entry is not JS consumption. No pending/absence conclusions
    for missed entries, changed identities, unsupported layouts or reused promises.
    """
    def __init__(self, emit):
        self.emit = emit
        self.requests, self.conditions, self.used_promises = {}, {}, set()
        self.serial = 0
        self.phase = 'database'
        self.counts = {}
        self.ready = None
        self.closed_requests = []

    @staticmethod
    def key(identity, module, context):
        return (identity['generation'], identity['user'], identity['pui_context'], module, context)

    def request(self, reader, regs, vsh_base, now):
        ret = reader.q(regs['rsp'], 'execute.caller') - vsh_base
        op = CALLERS.get(ret)
        if op is None:
            # No module/context/promise dereference on an unreviewed operation.
            # Only an exact audited caller gets a method name/RVA in output.
            raise ScopeExcluded('caller_not_traced',
                                caller=EXCLUDED_CALLERS.get(ret, 'unrecognized'),
                                caller_rva=hex(ret) if ret in EXCLUDED_CALLERS else None)
        module, context, promise = regs['rdi'], regs['rsi'] & 0xffffffff, regs['rcx']
        reader.typed(module, 'ReactNative.Modules.Vsh.Database.DatabaseModule', 'execute.module')
        identity = reader.identity(reader.q(module + 0x28, 'execute.context'))
        key = self.key(identity, module, context)
        # Invalidate before decoding: a failed condition must never leave old scope.
        if op == 'condition':
            self.conditions.pop(key, None)
            prior_counts = self.counts.pop(key[:4], {})
        closure = reader.q(regs['rdx'] + 0x20, 'execute.arguments')
        require(reader.q(closure + (0x20 if op == 'condition' else 0x18)) == promise,
                'closure_promise_mismatch')
        argument = reader.q(closure + 0x10)
        scope, home_items, revision = None, False, False
        if op == 'condition':
            try:
                scope = reader.condition(argument)
                require(scope['user'] == identity['user'], 'condition_user_mismatch')
            except DecodeError:
                self.counts.pop(key[:4], None)
                raise
            prior_counts.pop(scope['space'], None)
            self.counts[key[:4]] = prior_counts
        elif op == 'items':
            home_items = reader.columns(argument)
        else:
            f = reader.fields(argument)
            if op == 'items2':
                home_items = ('func' in f and reader.value(f['func']) == 'GetItems'
                              and 'itemProps' in f and reader.columns(f['itemProps']))
            else:
                # Native GetScalar2 receives the wrapper's array, not the
                # caller-facing scalarProp (Vsh+196801, Home HBC #4292).
                if 'scalarProps' in f:
                    count, array = reader.children(f['scalarProps'], True)
                    revision = count == 1 and reader.value(reader.q(array + 0x20)) == 'revision'
        if promise in self.used_promises:
            self.requests.pop(promise, None)
            raise DecodeError('promise_reused')
        require(len(self.used_promises) < 1024, 'request_budget')
        self.used_promises.add(promise)
        self.serial += 1
        if op != 'condition':
            scope = self.conditions.get(key)
        row = dict(event='request', request=self.serial, operation=op, **identity,
                   module=hex(module), context=context, promise=hex(promise),
                   revision=revision, home_columns=home_items, condition=scope,
                   observed_at=now)
        # Pending condition is retained by identity; acceptance requires its Resolve.
        if op == 'condition':
            scope = dict(scope, epoch=self.serial, settled=False)
            row['condition'] = scope
            self.conditions[key] = scope
        self.requests[promise] = dict(row=row, key=key, scope=scope, started=now,
                                      module=module, bridge=reader.q(module + 0x28))
        self.emit(row)

    def settlement(self, reader, regs, outcome, now):
        promise = regs['rdi']
        req = self.requests.pop(promise, None)
        if req is None:
            return
        row, key, scope = req['row'], req['key'], req['scope']
        result = dict(event='settlement', request=row['request'], promise=hex(promise),
                      generation=row['generation'], user=row['user'], context=row['context'],
                      outcome=outcome, elapsed=now - req['started'])
        try:
            identity = reader.identity(reader.q(req['module'] + 0x28))
            require(self.key(identity, req['module'], row['context']) == key, 'generation_changed')
            require(reader.byte(req['module'] + 0x40) == 0, 'module_disposed')
            callback = reader.q(promise + (0x10 if outcome == 'resolve' else 0x18), 'settlement.callback')
            result['callback_present'] = bool(callback)
            require(callback != 0, 'callback_absent')
            if row['operation'] == 'condition':
                require(self.conditions.get(key) is scope, 'condition_superseded')
                scope['settled'] = outcome == 'resolve'
            elif row['home_columns'] and outcome == 'resolve':
                reader.site = 'settlement.items'
                count = reader.item_count(regs['rsi'])
                result['item_count'] = count
                require(scope is not None and scope is self.conditions.get(key)
                        and scope['settled'], 'condition_not_correlated')
                result.update(space=scope['space'], condition_epoch=scope['epoch'])
                group = self.counts.setdefault(key[:4], {})
                group[scope['space']] = (count, row['request'])
                if 'game' in group and 'media' in group and group['game'][0] > 0:
                    self.ready = dict(generation=row['generation'], user=row['user'],
                                      pui_context=row['pui_context'], requests=group.copy())
            elif row['revision'] and outcome == 'resolve':
                reader.site = 'settlement.revision'
                f = reader.fields(regs['rsi'])
                # Promise entry precedes the JS wrapper's scalarMap unwrapping.
                require('ctxId' in f and reader.value(f['ctxId']) == row['context'],
                        'revision_context_mismatch')
                require('scalarMap' in f, 'scalar_map_missing')
                scalars = reader.fields(f['scalarMap'])
                require('revision' in scalars, 'revision_missing')
                value = reader.value(scalars['revision'])
                require(type(value) is int, 'revision_type')
                result['revision_value'] = value
        except DecodeError as error:
            result['decode_status'] = str(error)
            result['diagnostic'] = reader.diagnostic(error)
        self.emit(result)

    def timing(self, reader, regs, predicate=False):
        if predicate:
            closure = reader.q(regs['rbp'] - 0x18)
            view, manager = reader.q(closure + 0x10), reader.q(closure + 0x20)
            aux = reader.q(view + 0x30)
            require(reader.byte(aux + 0x38) == 1, 'tag_unavailable')
            identity = reader.identity(reader.q(aux + 0x10))
            tag = reader.i(aux + 0x3c)
            row = dict(event='predicate', result=bool(regs['rax'] & 255), view=hex(view),
                       manager=hex(manager), target=hex(reader.q(reader.q(manager) + 0x188)))
        else:
            event, emitter = regs['rdi'], regs['rsi']
            reader.typed(event, 'ReactNative.UIManager.Events.ReactPerformanceTimingEvent', 'dispatch.event')
            reader.typed(emitter, 'ReactNative.UIManager.Events.RCTEventEmitter', 'dispatch.emitter')
            handler = reader.q(emitter + 0x10, 'dispatch.handler')
            instance = reader.q(handler + 0x10, 'dispatch.instance')
            identity = reader.identity(reader.instance_context(instance, 'dispatch.instance'))
            tag = reader.i(event + 0x14)
            name = reader.string(reader.q(event + 0x20, 'dispatch.name'),
                                 {'interactive', 'firstPaint', 'widgetReady'}, 'dispatch.name')
            require(name is not None, 'timing_name')
            row = dict(event='dispatch', name=name, emitter=hex(emitter))
        focused = bool(self.ready and all(identity[k] == self.ready[k]
                       for k in ('generation', 'user', 'pui_context')))
        self.emit(dict(row, **identity, tag=tag, phase=self.phase, focus_match=focused))

    def outstanding(self, now):
        return self.closed_requests + [dict(request=r['row']['request'], generation=r['row']['generation'],
                     user=r['row']['user'], operation=r['row']['operation'],
                     context=r['row']['context'], promise=r['row']['promise'],
                     observed_unsettled_seconds=now - r['started']) for r in self.requests.values()]

    def close_coverage(self, now, reason):
        # Lifecycle draining deliberately does not decode guest objects. Freeze
        # observation durations and invalidate associations before that gap;
        # a settlement missed during drain must not match a later reused pointer.
        self.closed_requests = [dict(row, coverage_closed=row.get('coverage_closed', reason))
                                for row in self.outstanding(now)]
        self.requests.clear()
