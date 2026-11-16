"""Native particle mode/gate/submission observations; no scheduling-success inference."""
from home_gate_trace_1360 import require, ScopeExcluded
from home_transition_trace_1360 import signed

PROBES = {'particle_mode': 0xeb590, 'job_gate': 0xe7fde,
          'job_submit': 0x155fd0, 'job_submit_return': 0xe80eb}
ANCHORS = {
    0xeb590: '8b87d407000039f00f84ef000000',
    0xc082e: 'e85dad0200',
    0xe7fde: '80bf20070000010f85000100004883bba8070000000f85f2000000',
    0x155fd0: '4889fe488b3dbe6c25014885ff0f852d364600b8ffffffffc3',
    0xe80e3: '4c89e7e8e5de0600',
    0xe80eb: 'c7830807000000000000',
    0xeb5d1: '41b800000101',
}


def mode_write(old, requested):
    """None means this setter leaves the gate unchanged, not false."""
    old, requested = old & 0xffffffff, requested & 0xffffffff
    if old == requested or (old != 0 and requested in (3, 4)):
        return None
    return int(requested in (3, 4))


def audit(image):
    for address, text in ANCHORS.items():
        raw = bytes.fromhex(text)
        require(image.at(address, len(raw)) == raw, 'job_instruction_abi')


class Model:
    ready = False
    phase = 'job'

    def __init__(self, emit, base):
        self.emit, self.base = emit, base
        self.active, self.poisoned = {}, set()
        self.serial, self.closed = 0, False

    def thread_boundary(self, tid):
        if tid in self.active:
            self.emit(dict(event='job_unpaired', tid=tid, invocation=self.active[tid]['id']))
        self.active.pop(tid, None)
        self.poisoned.discard(tid)

    def close_coverage(self, now, reason):
        self.closed = True

    def outstanding(self, now):
        return [dict(tid=t, invocation=v['id'], stages=['job_submit']) for t, v in self.active.items()]

    def owner(self, r, p):
        require(r.q(p) == self.base + 0x1169d68, 'job_owner_vtable')
        return p

    def job(self, r, owner):
        job = r.q(owner + 0x7a8)
        require(r.q(job) == self.base + 0x1169df0 and r.q(job + 0x38) == owner,
                'job_fresh_backlink')
        return job

    def hit(self, name, r, g, now, tid=None):
        require(tid is not None and name in PROBES, 'job_probe_thread')
        if self.closed or tid in self.poisoned:
            raise ScopeExcluded('job_coverage_closed')
        try:
            self.decode(name, r, g, tid)
        except Exception:
            self.active.pop(tid, None)
            raise

    def decode(self, name, r, g, tid):
        if name == 'particle_mode':
            require(r.q(g['rsp']) == self.base + 0xc0833, 'job_mode_caller')
            owner = self.owner(r, g['rdi'])
            old, requested = r.i(owner + 0x7d4), g['rsi'] & 0xffffffff
            self.emit(dict(event=name, tid=tid, previous_mode=old, requested_mode=requested,
                           gate_before=r.byte(owner + 0x720), predicted_gate_write=mode_write(old, requested)))
            return
        if name == 'job_gate':
            require(g['rdi'] == g['rbx'], 'job_gate_registers')
            owner = self.owner(r, g['rdi'])
            flag, job = r.byte(owner + 0x720), r.q(owner + 0x7a8)
            self.emit(dict(event=name, tid=tid, gate_byte=flag, job_present=bool(job),
                           creation_eligible_snapshot=flag == 1 and job == 0))
            return
        if name == 'job_submit':
            # Shared helper has many unrelated callers. Exclude before reading
            # the caller's preserved RBX or interpreting its reference pointer.
            if r.q(g['rsp']) != self.base + 0xe80eb:
                raise ScopeExcluded('job_foreign_submit')
            if tid in self.active:
                self.poisoned.add(tid)
                require(False, 'job_nested_submit')
            owner = self.owner(r, g['rbx'])
            require(g['rdi'] == owner + 0x7a8, 'job_reference_argument')
            job = self.job(r, owner)
            scheduler = r.q(self.base + 0x13acc98)
            self.serial += 1
            require(self.serial <= 1024, 'job_invocation_budget')
            self.active[tid] = dict(id=self.serial, owner=owner, job=job, scheduler=scheduler,
                                    stack=g['rsp'], frame=g['rbp'], caller=r.q(g['rbp'] + 8))
            self.emit(dict(event=name, tid=tid, invocation=self.serial,
                           scheduler_present=bool(scheduler),
                           registered_workers=r.q(scheduler + 0x30) if scheduler else None,
                           job_state_before=r.i(job + 0x28)))
            return
        if tid not in self.active:
            # Both skipped creation branches reach this address too. RAX has
            # no submission-result meaning without the observed helper call.
            raise ScopeExcluded('job_no_observed_submit')
        v = self.active[tid]
        require(g['rsp'] == v['stack'] + 8 and g['rbp'] == v['frame'] and
                r.q(g['rbp'] + 8) == v['caller'], 'job_return_frame')
        owner = self.owner(r, g['rbx'])
        require(owner == v['owner'] and self.job(r, owner) == v['job'], 'job_return_identity')
        require(r.q(self.base + 0x13acc98) == v['scheduler'], 'job_scheduler_changed')
        self.active.pop(tid)
        self.emit(dict(event=name, tid=tid, invocation=v['id'], result=signed(g['rax']),
                       job_state_after=r.i(v['job'] + 0x28)))
