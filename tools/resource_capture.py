"""Read-only 13.60 ResourceArbitrator evidence; never atomic or lifetime-pinned.

The caller owns process/image/code admission and the external capture timeout.
Reader(address, size) returns ReadResult, without retries or target writes.
"""
from dataclasses import dataclass
import hashlib
import json
import struct
import time


CONFIG_SHA256 = 'd9479085076615484a2a443b7b1ee6725a9f7701225696a63581a636b328c71a'
MAX_READ = 96
MAX_ATTEMPTS = 904
MAX_BYTES = 63872
MAX_COMPONENTS = 64
MAX_STATES = 256
MAX_COMPONENT_STATES = 32
ADDRESS_LIMIT = 1 << 40  # Current emulator GuestHeap admission, not universal ABI.


@dataclass(frozen=True)
class ReadResult:
    data: bytes
    complete: bool = True
    error: str = ''


class CaptureStop(Exception):
    pass


def _u64(data, offset=0):
    return struct.unpack_from('<Q', data, offset)[0]


def _u32(data, offset=0):
    return struct.unpack_from('<I', data, offset)[0]


def _expected(config):
    """Pure normalization; absence of an unproven default means unassessed."""
    components = []
    for c in sorted(config['clientComponents'], key=lambda v: v['id']):
        item = dict(id=c['id'], name_hex=c['name'].encode('utf-8').hex(), states=[])
        if 'needConfirm' in c:
            item['needConfirm'] = int(c['needConfirm'])
        for s in c['states']:
            state = dict(name_hex=s['name'].encode('utf-8')[:31].hex())
            for key in ('vcn', 'vcnType', 'acp', 'mem', 'priority',
                        'needBigAppSuspended', 'needMediaAppKilled', 'need8kReady'):
                if key in s:
                    state[key] = int(s[key])
            # Native explicit branches: C+876050, +876160, +8761a0.
            state['scaninType'] = int(s.get('scaninType', 0))
            state['forcedConfirmResultEncoded'] = 2-int(s['forcedConfirmResult']) if 'forcedConfirmResult' in s else 0
            if 'kickPriority' in s:
                state['kickPriority'] = s['kickPriority']
            elif 'priority' in s:
                state['kickPriority'] = s['priority']
            item['states'].append(state)
        components.append(item)
    return dict(components=components, fields=dict(
        version=config['version'], components=len(components),
        exclusive_groups=len(config['exclusiveGroups']), first_win_groups=len(config['firstWinGroups']),
        vcn_budget=config['maxVcnBudget'], acp_budget=config['maxAcpBudget'], memory_budget=config['maxMemBudget']))


def load_expected(path):
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != CONFIG_SHA256:
        raise ValueError('unrecognized ResourceArbitrator config SHA256')
    result = _expected(json.loads(data))
    result.update(config_sha256=digest, source_path=str(path.resolve()), cronos=0,
                  evidence_kind='expected_local_file_not_observed')
    return result


def collect_resource(reader, core_base, expected, *, clock=time.monotonic, wall_clock=time.time_ns):
    """One bounded traversal plus same-range rereads. All errors remain evidence.

    The two-second cooperative deadline cannot interrupt a blocking reader call.
    No retry, restart, allocation-size read, parent traversal, or ownership acquire.
    """
    start = clock()
    result = dict(atomic=False, lifetime_pinned=False, snapshots=[], components=[],
                  expected=expected, traversal_complete=False, structural_checks=False,
                  config_content_matches=None, all_rechecked_bytes_equal=None,
                  publication_unchanged_during_read=None, stop_reason=None,
                  recheck_stop_reason=None, read_attempts=0, requested_bytes=0,
                  transferred_bytes=0, rechecks=[], limits=dict(
                      attempts=MAX_ATTEMPTS, requested_bytes=MAX_BYTES, per_read=MAX_READ,
                      cooperative_seconds=2, components=MAX_COMPONENTS,
                      states_per_component=MAX_COMPONENT_STATES, states=MAX_STATES))
    saved = []
    regions = []
    name_regions = []

    def require(condition, reason):
        if not condition:
            raise CaptureStop(reason)

    def address_ok(address, size, alignment=8):
        require(isinstance(address, int) and 0x10000 <= address < ADDRESS_LIMIT,
                'address_out_of_range')
        require(0 < size <= MAX_READ and size <= ADDRESS_LIMIT-address, 'range_or_size_invalid')
        require(address % alignment == 0, 'misaligned_pointer')

    def claim(address, size, role):
        address_ok(address, size)
        require(all(address+size <= a or a+n <= address for a, n, _ in regions),
                'overlapping_object_ranges:' + role)
        require(all(address+size <= a or a+n <= address for a, n in name_regions),
                'object_overlaps_heap_name:' + role)
        regions.append((address, size, role))

    def claim_name(address, size):
        address_ok(address, size, 1)
        require(all(address+size <= a or a+n <= address for a, n, _ in regions),
                'heap_name_overlaps_object')
        name_regions.append((address, size))  # Shared name buffers are permitted.

    def read(label, address, size, *, alignment=8, recheck=False):
        address_ok(address, size, alignment)
        require(clock()-start < 2, 'deadline')
        # Reserve one attempt and its bytes for each initial read's reread,
        # including failed initial attempts (which will not actually be reread).
        factor = 1 if recheck else 2
        if recheck:
            calls = result['read_attempts']+1
            byte_count = result['requested_bytes']+size
        else:
            calls = factor*(result['read_attempts']+1)
            byte_count = factor*(result['requested_bytes']+size)
        require(calls <= MAX_ATTEMPTS and byte_count <= MAX_BYTES, 'read_budget')
        entry = dict(label=label, address=hex(address), requested=size,
                     phase='recheck' if recheck else 'initial',
                     started_ns=wall_clock(), elapsed_start=clock()-start)
        result['read_attempts'] += 1
        result['requested_bytes'] += size
        result['snapshots'].append(entry)
        try:
            answer = reader(address, size)
            if not isinstance(answer, ReadResult) or not isinstance(answer.data, bytes):
                raise TypeError('reader must return ReadResult with bytes')
            entry.update(transferred=len(answer.data), hex=answer.data[:size].hex(),
                         available=answer.complete and len(answer.data) == size,
                         error=answer.error)
        except Exception as error:
            entry.update(transferred=0, hex='', available=False,
                         error=type(error).__name__ + ': ' + str(error))
        entry.update(finished_ns=wall_clock(), elapsed_end=clock()-start)
        result['transferred_bytes'] += entry['transferred']
        require(entry['available'], 'read_unavailable:' + label)
        data = answer.data
        if not recheck:
            saved.append((label, address, size, alignment, data))
        return data

    def text(raw, label):
        require(b'\0' not in raw, 'embedded_nul:' + label)
        try:
            return raw.decode('utf-8', errors='strict')
        except UnicodeDecodeError:
            raise CaptureStop('invalid_utf8:' + label) from None

    def state_record(data, address):
        name = data[0x10:0x30]
        require(name[-1] == 0, 'unterminated_state_name')
        raw_name = name.split(b'\0', 1)[0]
        values = dict(address=hex(address), name=text(raw_name, 'state'), name_hex=raw_name.hex())
        for key, off in (('vcn', 0x30), ('vcnType', 0x38), ('acp', 0x3c),
                         ('mem', 0x40), ('priority', 0x44), ('kickPriority', 0x48)):
            values[key] = struct.unpack_from('<i', data, off)[0]
        values['scaninType'] = data[0x34]
        for key, off in (('needBigAppSuspended', 0x4c), ('needMediaAppKilled', 0x4d),
                         ('need8kReady', 0x4e), ('forcedConfirmResultEncoded', 0x4f)):
            values[key] = data[off]
        require(all(data[off] <= 1 for off in (0x4c, 0x4d, 0x4e)) and data[0x4f] <= 2,
                'invalid_state_flag_encoding')
        return values

    try:
        owner = _u64(read('owner', core_base+0x20f8fb0, 8))
        result['owner'] = hex(owner)
        require(owner != 0, 'not_published')
        address_ok(owner, 8)
        pair_address = owner+0x738
        pair = read('publication', pair_address, 16)
        rule, control = struct.unpack('<QQ', pair)
        result.update(rule=hex(rule), control=hex(control))
        require(rule != 0 or control == 0, 'publication_inconsistent')
        require(rule != 0, 'not_published')
        require(control != 0, 'publication_inconsistent')
        address_ok(control, 8)  # No ownership/control-block reads or writes.
        claim(core_base+0x20f8fb0, 8, 'owner_slot')
        claim(pair_address, 16, 'publication')
        claim(rule, 72, 'rule')
        header = read('rule_header', rule, 72)
        fields = dict(version=_u32(header, 0x44), components=_u64(header, 0x28),
                      exclusive_groups=_u64(header, 0x18), first_win_groups=_u64(header, 8),
                      vcn_budget=_u32(header, 0x30), acp_budget=_u32(header, 0x3c),
                      memory_budget=_u32(header, 0x40))
        result['fields'] = fields
        require(fields['components'] <= MAX_COMPONENTS, 'component_count_cap')
        head = _u64(header, 0x20)
        claim(head, 32, 'tree_sentinel')
        head_data = read('tree_sentinel', head, 32)
        minimum, root, maximum = struct.unpack_from('<QQQ', head_data)
        require(head_data[0x18:0x1a] == b'\1\1', 'tree_sentinel_flags')
        require((root == head) == (fields['components'] == 0), 'empty_tree_count')
        tree = {}
        stack = [] if root == head else [(root, head, -1, 1 << 32, 1)]
        total_states = 0
        declared_states = 0
        seen_states = set()
        while stack:
            node, parent, lower, upper, depth = stack.pop()
            require(node not in tree, 'tree_cycle_or_reuse')
            require(len(tree) < MAX_COMPONENTS and depth <= MAX_COMPONENTS, 'tree_traversal_cap')
            claim(node, 96, 'component')
            data = read('component:' + hex(node), node, 96)
            left, native_parent, right = struct.unpack_from('<QQQ', data)
            color, isnil, identity = data[0x18], data[0x19], _u32(data, 0x20)
            require(isnil == 0 and color in (0, 1), 'component_flags')
            require(native_parent == parent, 'tree_parent_mismatch')
            require(lower < identity < upper, 'tree_key_order')
            tree[node] = (left, right, color, identity)
            length, capacity = _u64(data, 0x38), _u64(data, 0x40)
            require(length <= 64 and length <= capacity, 'component_name_bounds')
            if capacity < 16:
                require(capacity == 15 and length <= 15, 'inline_name_bounds')
                name = data[0x28:0x28+length+1]
            else:
                name_address = _u64(data, 0x28)
                claim_name(name_address, length+1)
                name = read('component_name:' + hex(node), name_address, length+1, alignment=1)
            require(name[-1] == 0, 'unterminated_component_name')
            raw_name = name[:-1]
            require(data[0x48] <= 1, 'invalid_needConfirm_encoding')
            item = dict(address=hex(node), id=identity, name=text(raw_name, 'component'),
                        name_hex=raw_name.hex(), needConfirm=data[0x48], states=[])
            result['components'].append(item)
            sentinel, count = _u64(data, 0x50), _u64(data, 0x58)
            item['declared_state_count'] = count
            declared_states += count
            require(count <= MAX_COMPONENT_STATES and declared_states <= MAX_STATES, 'state_count_cap')
            claim(sentinel, 16, 'state_sentinel')
            first, last = struct.unpack('<QQ', read('state_sentinel:' + hex(node), sentinel, 16))
            current, previous = first, sentinel
            while current != sentinel:
                require(current not in seen_states, 'state_cycle_or_reuse')
                require(len(item['states']) < MAX_COMPONENT_STATES and total_states < MAX_STATES,
                        'state_traversal_cap')
                claim(current, 80, 'state')
                value = read('state:' + hex(current), current, 80)
                next_node, prev_node = struct.unpack_from('<QQ', value)
                require(prev_node == previous, 'state_previous_mismatch')
                seen_states.add(current)
                total_states += 1
                item['states'].append(state_record(value, current))
                previous, current = current, next_node
            require(previous == last, 'state_tail_mismatch')
            require(len(item['states']) == count, 'state_count_mismatch')
            # The stack contains at most two children per admitted component.
            if right != head:
                stack.append((right, node, identity, upper, depth+1))
            if left != head:
                stack.append((left, node, lower, identity, depth+1))
        require(len(tree) == fields['components'], 'component_count_mismatch')
        if tree:
            require(tree[root][2] == 1, 'root_not_black')
            ordered = sorted(tree, key=lambda a: tree[a][3])
            require(minimum == ordered[0] and maximum == ordered[-1], 'tree_extrema_mismatch')
        else:
            require(minimum == head and maximum == head, 'empty_tree_links')
        # Postorder over already captured nodes only; no new target reads.
        black_height = {head: 1}
        pending = [(root, False)] if tree else []
        while pending:
            node, ready = pending.pop()
            left, right, color, _ = tree[node]
            if not ready:
                pending.append((node, True))
                pending.extend((child, False) for child in (right, left) if child != head)
                continue
            require(black_height[left] == black_height[right], 'tree_black_height')
            if color == 0:
                require(all(child == head or tree[child][2] == 1 for child in (left, right)), 'tree_red_red')
            black_height[node] = black_height[left]+color
        result['components'].sort(key=lambda v: v['id'])
        result.update(traversal_complete=True, structural_checks=True, observed_state_count=total_states)
        differences = []
        for key, value in expected['fields'].items():
            if fields.get(key) != value:
                differences.append('fields.'+key)
        actual = result['components']
        wanted = expected['components']
        if [c['id'] for c in actual] != [c['id'] for c in wanted]:
            differences.append('component_ids')
        by_id = {c['id']: c for c in actual}
        unassessed = []
        decoded_keys = set(('vcn', 'scaninType', 'vcnType', 'acp', 'mem', 'priority', 'kickPriority',
                            'needBigAppSuspended', 'needMediaAppKilled', 'need8kReady', 'forcedConfirmResultEncoded'))
        for c in wanted:
            prefix = 'component['+str(c['id'])+']'
            if c['id'] not in by_id:
                continue
            observed = by_id[c['id']]
            for key in ('name_hex', 'needConfirm'):
                if key in c and observed[key] != c[key]:
                    differences.append(prefix+'.'+key)
            if 'needConfirm' not in c:
                unassessed.append(prefix+'.needConfirm')
            if len(c['states']) != len(observed['states']):
                differences.append(prefix+'.state_count')
            for index, (want, have) in enumerate(zip(c['states'], observed['states'])):
                p = prefix+'.state['+str(index)+']'
                differences.extend(p+'.'+key for key, value in want.items() if have.get(key) != value)
                unassessed.extend(p+'.'+key for key in sorted(decoded_keys-want.keys()))
        result.update(config_content_matches=not differences, config_differences=differences,
                      unassessed_fields=unassessed,
                      comparison_scope='component/state explicit fields and verified defaults; group counts only')
        result['stop_reason'] = 'complete'
    except CaptureStop as error:
        result['stop_reason'] = str(error)
    except Exception as error:
        result['stop_reason'] = 'collector_error:' + type(error).__name__ + ':' + str(error)
    finally:
        # Reverse original reads, without discovering/chasing any changed pointers.
        for label, address, size, alignment, original in reversed(saved):
            check = dict(label=label, address=hex(address), equal=None)
            result['rechecks'].append(check)
            try:
                check['equal'] = read(label, address, size, alignment=alignment, recheck=True) == original
            except CaptureStop as error:
                check['error'] = str(error)
                result['recheck_stop_reason'] = str(error)
                # An unavailable range need not prevent root rechecks. Budget or
                # deadline exhaustion does; no reader retries are made.
                if str(error) in ('deadline', 'read_budget'):
                    break
        checks = {v['label']: v['equal'] for v in result['rechecks']}
        result['all_rechecked_bytes_equal'] = (len(result['rechecks']) == len(saved)
                                               and all(v['equal'] is True for v in result['rechecks'])) if saved else None
        result['root_rechecks'] = {key: checks.get(key) for key in ('tree_sentinel', 'rule_header', 'publication', 'owner')}
        if 'publication' in checks and 'owner' in checks:
            result['publication_unchanged_during_read'] = checks['publication'] is True and checks['owner'] is True
        result['elapsed_seconds'] = clock()-start
    return result
