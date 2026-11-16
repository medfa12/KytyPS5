"""Offline synthetic memory, provenance, privacy and code-migration controls."""
import json
from pathlib import Path
import struct
import tempfile
import unittest

from home_gate_trace_1360 import Reader, DecodeError, ScopeExcluded
from test_home_gate_trace_1360 import Memory
import retail_plus_trace_1360 as t
import capture_retail_plus_1360 as cli


class PlusTests(unittest.TestCase):
    def setUp(self):
        self.m = Memory()
        self.bridge_ctx,self.context,self.instance = self.m.identity(user=0x10010029)
        self.bases = dict(host=0x140000000,pui=0xdc0000000,plus=0xe10000000,
                          hermes=0x980000000,extensions=0x9a0000000)
        self.observer = t.Observer({'pui': Path('C:/fixture/ReactNative.PUI.dll.sprx'),
                                   'plus': Path('C:/fixture/ReactNative.Modules.Vsh.dll.sprx')},'a'*32)
        self.rows=[]
        self.model=self.observer.model(self.rows.append,self.bases)
        self.model.probes=dict(t.NATIVE)
        self.module=self.m.obj('ReactNative.Modules.Vsh.PSN.EntitlementCSIModule')
        self.m.q(self.module+0x28,self.bridge_ctx)
        self.promise=self.m.obj('ReactNative.Bridge.Promise')
        self.callbacks=[]
        for offset,ident in ((0x10,100),(0x18,101)):
            cb=self.m.obj('.Callback')
            self.m.q(cb+0x10,self.m.weak(self.instance));self.m.i(cb+0x18,ident)
            self.m.q(self.promise+offset,cb);self.callbacks.append(cb)
        at=self.bases['pui']+0x127e64
        self.m.put(at,bytes.fromhex(t.CHECKS['pui'][0x127e64]))
        got=at+7+struct.unpack('<i',self.m.read(at+3,4))[0]
        self.m.q(got,self.q(self.callbacks[0]))
        self.param=self.m.object({'userId':self.m.value(0x10010029)})
        self.worker=self.m.obj('.<>c__DisplayClass79_0')
        self.m.q(self.bases['plus']+0x5798e0,self.q(self.worker))
        self.m.q(self.worker+0x18,self.promise);self.m.q(self.worker+0x20,self.module)
        self.stack=self.m.alloc();self.m.q(self.stack,0x1000abcd)
        self.regs=dict(rdi=self.module,rsi=self.param,rdx=self.promise,r15=self.worker,
                       rax=0,r14=0,rsp=self.stack,rbp=self.stack-8)
        data=self.q(self.context+0x30)
        bridge=self.m.obj('ReactNative.Bridge.ReactBridge')
        self.executor=self.m.obj('ReactNative.JavaScriptCore.Executor.JSJavaScriptExecutor')
        self.m.q(data+0x18,bridge);self.m.q(bridge+0x18,self.executor)
        native,impl,runtime=(self.m.alloc() for _ in range(3))
        vt=self.bases['extensions']+0x95320
        self.m.q(self.executor+0x80,native);self.m.q(native,vt)
        self.m.q(vt+0x28,self.bases['extensions']+0x2c1f0)
        self.m.q(vt+0x30,self.bases['extensions']+0x2c8d0)
        self.m.q(native+0x78,impl);self.m.q(impl,self.bases['hermes']+0x26c348)
        self.m.q(impl+0x70,runtime)
        self.delivery=self.m.obj('.<>c__DisplayClass47_0')
        self.m.q(self.delivery+0x10,self.m.alloc());self.m.q(self.delivery+0x18,self.executor)
        self.m.i(self.delivery+0x20,100)

    def q(self,p):return struct.unpack('<Q',self.m.read(p,8))[0]

    def hit(self,name,**registers):
        self.model.hit(name,Reader(self.m.read,self.m.mono),dict(self.regs,**registers),1,tid=10)

    def acquire(self):self.hit('plus_entry')

    def test_full_chain_excludes_membership_and_payload(self):
        self.acquire()
        self.assertEqual(list(self.observer.transition()[0])[0],'plus_delivery')
        self.hit('plus_worker',rdi=self.worker)
        self.hit('plus_service_call',rdi=0x10010029)
        self.hit('plus_service_result',rax=0)
        self.hit('plus_service_done',rax=0,r14=0)
        self.hit('plus_resolve',rdi=self.promise,rsi=0xdeadbeef) # unreadable payload must not be read
        self.hit('plus_delivery',rdi=self.delivery)
        self.hit('plus_delivery_return',rsp=self.stack-8,rbp=self.stack-8)
        self.assertTrue(self.model.complete)
        text=json.dumps(self.rows)
        self.assertNotIn('deadbeef',text);self.assertNotIn('3735928559',text)
        self.assertEqual(self.rows[-1]['js_consumption'],'unknown')

    def test_rejection_can_bypass_worker(self):
        self.acquire();self.hit('plus_reject',rdi=self.promise,rsi=0xdeadbeef)
        self.m.i(self.delivery+0x20,101)
        self.hit('plus_delivery',rdi=self.delivery)
        self.assertEqual(self.rows[-1]['side'],'reject')

    def test_foreign_callback_excluded(self):
        self.acquire();self.m.i(self.delivery+0x20,900)
        with self.assertRaises(ScopeExcluded):self.hit('plus_delivery',rdi=self.delivery)

    def test_foreign_promise_excluded(self):
        self.acquire();self.m.i(self.callbacks[0]+0x18,800)
        with self.assertRaises(ScopeExcluded):self.hit('plus_resolve',rdi=self.promise)

    def test_wrong_user_param_rejected(self):
        self.regs['rsi']=self.m.object({'userId':self.m.value(7)})
        with self.assertRaisesRegex(DecodeError,'plus_param_user'):self.acquire()

    def test_reused_callback_pair_rejected(self):
        self.m.i(self.callbacks[1]+0x18,100)
        with self.assertRaisesRegex(DecodeError,'plus_callback_owner_mismatch'):self.acquire()

    def test_wrong_module_owner_rejected(self):
        other,_,_=self.m.identity(user=0x10010029,uid=b'2'*16)
        self.m.q(self.module+0x28,other)
        with self.assertRaisesRegex(DecodeError,'plus_entry_owner'):self.acquire()

    def test_nested_worker_type_requires_exact_vtable(self):
        self.acquire();self.m.q(self.bases['plus']+0x5798e0,0x123456)
        with self.assertRaisesRegex(DecodeError,'plus_worker_type'):self.hit('plus_worker',rdi=self.worker)

    def test_native_cancellation_control_only(self):
        self.acquire();self.hit('plus_worker',rdi=self.worker)
        self.hit('plus_service_call',rdi=0x10010029)
        self.hit('plus_service_result',rax=0xffffffff)
        self.hit('plus_service_done',rax=1,r14=0xffffffff)
        self.assertTrue(self.rows[-1]['cancelled']);self.assertTrue(self.rows[-1]['service_failed'])
        self.assertIsNone(self.model.req['settlement'])

    def test_service_wrong_user(self):
        self.acquire();self.hit('plus_worker',rdi=self.worker)
        with self.assertRaisesRegex(DecodeError,'plus_service_user'):self.hit('plus_service_call',rdi=7)

    def test_delivery_without_settlement_is_not_success(self):
        self.acquire()
        with self.assertRaisesRegex(DecodeError,'plus_delivery_order_or_reuse'):self.hit('plus_delivery',rdi=self.delivery)

    def test_duplicate_settlement_poisoned(self):
        self.acquire();self.hit('plus_resolve',rdi=self.promise)
        with self.assertRaisesRegex(DecodeError,'plus_duplicate_settlement'):self.hit('plus_reject',rdi=self.promise)

    def test_same_callback_id_on_foreign_executor_excluded(self):
        self.acquire();self.hit('plus_resolve',rdi=self.promise)
        self.m.q(self.delivery+0x18,self.m.obj('ReactNative.JavaScriptCore.Executor.JSJavaScriptExecutor'))
        with self.assertRaisesRegex(ScopeExcluded,'plus_other_executor'):self.hit('plus_delivery',rdi=self.delivery)
        self.assertFalse(self.model.req['delivery'])

    def test_return_wrong_frame(self):
        self.acquire();self.hit('plus_resolve',rdi=self.promise);self.hit('plus_delivery',rdi=self.delivery)
        with self.assertRaisesRegex(DecodeError,'plus_callback_return_frame'):self.hit('plus_delivery_return')

    def test_owner_termination_at_delivery(self):
        self.acquire();self.hit('plus_resolve',rdi=self.promise);self.m.put(self.context+0x92,b'\x01')
        with self.assertRaisesRegex(DecodeError,'plus_context_terminated'):self.hit('plus_delivery',rdi=self.delivery)

    def test_owned_thread_exit_fails_closed(self):
        self.acquire();self.hit('plus_worker',rdi=self.worker)
        with self.assertRaisesRegex(DecodeError,'plus_owned_thread_lifecycle_gap'):self.model.thread_boundary(10)

    def test_outstanding_never_claims_pending(self):
        self.acquire()
        self.assertEqual(self.model.outstanding(20)[0]['native_promise_state'],'not inferred from absence')

    def test_lazy_mapping_checks_exact_path(self):
        self.bases.pop('pui');self.bases.pop('plus')
        for key,size in (('pui',0x860000),('plus',0x5e4000)):
            p=self.m.alloc(512);text=str(self.observer.selected[key]).encode('utf-16-le');at=self.m.alloc(len(text))
            self.m.put(at,text);self.m.q(p+0x80,at);self.m.q(p+0x90,len(text)//2);self.m.q(p+0x98,len(text)//2)
            self.m.q(p+0xb0,0xdc0000000 if key=='pui' else 0xe10000000);self.m.q(p+0xc8,size)
            self.hit('loader',rax=p)
            probes,placements=self.observer.transition();self.bases.update(placements)
        self.assertEqual(probes,t.NATIVE)


class MigrationTests(unittest.TestCase):
    def test_partial_rearm_failure_journals_both_plans_for_cleanup(self):
        class Threads:
            saved={}
            def rearm(self,a):raise OSError('partial SetThreadContext failure')
        names=list('abcd');addresses=[1,2,3,4];probes={};retired={}
        updated={n:('x',a) for n,a in zip('efgh',[5,6,7,8])}
        with self.assertRaises(OSError):t.handoff(Threads(),names,addresses,probes,retired,updated,{'x':0},{})
        self.assertEqual(addresses,[5,6,7,8]);self.assertEqual(set(retired),{1,2,3,4})
        traps={a:n for n,a in enumerate(addresses)} | {a:s for a,(s,_) in retired.items()}
        self.assertEqual(set(traps),set(range(1,9)))

    def test_foreign_pending_cause_rejected_before_write(self):
        raw=bytearray(1232);struct.pack_into('<Q',raw,248,1);struct.pack_into('<Q',raw,104,0x4000)
        class Threads:
            saved={7:(8,None)}
            def context(self,h):return raw
            def rearm(self,a):raise AssertionError('must not write')
        with self.assertRaisesRegex(DecodeError,'plus_foreign_pending'):
            t.handoff(Threads(),list('abcd'),[1,2,3,4],{}, {},
                      dict(e=('x',5),b=('x',2),c=('x',3),d=('x',4)),{'x':0},{})

    def test_repeated_migration_keeps_queue_proofs(self):
        raw=bytearray(1232);struct.pack_into('<Q',raw,248,10)
        class Threads:
            saved={7:(8,None)}
            def context(self,h):return raw
            def rearm(self,a):self.plan=list(a)
        threads=Threads();names=list('abcd');addresses=[10,20,30,40];probes={};retired={}
        update={'e':('x',50),'f':('x',60),'g':('x',70),'h':('x',80)}
        pending=t.handoff(threads,names,addresses,probes,retired,update,{'x':0},{})
        self.assertEqual(pending,{(7,10):0})
        struct.pack_into('<Q',raw,248,60)
        update={'i':('x',90),'f':('x',60),'g':('x',70),'h':('x',80)}
        pending=t.handoff(threads,names,addresses,probes,retired,update,{'x':0},pending)
        self.assertEqual(pending,{(7,10):0,(7,60):1});self.assertEqual(len(retired),5)
        update['i']=('x',10)
        with self.assertRaisesRegex(DecodeError,'plus_retired_reuse'):
            t.handoff(threads,names,addresses,probes,retired,update,{'x':0},pending)

    def test_cross_slot_move_rejected_before_write(self):
        class Threads:
            saved={}
            def rearm(self,a):raise AssertionError('must not write')
        with self.assertRaisesRegex(DecodeError,'plus_slot_migration'):
            t.handoff(Threads(),list('abcd'),[1,2,3,4],{}, {},
                      dict(a=('x',2),b=('x',1),c=('x',3),d=('x',4)),{'x':0},{})


class ReceiptTests(unittest.TestCase):
    def rows(self):
        images={k:dict(sha256=h) for k,(_,h) in {**t.config.PINS,'plus':t.PLUS}.items()}
        images.update(home=dict(sha256=t.config.HOME_HASH),observer=dict(contract=cli.CONTRACT,sources=cli.sources()))
        return [dict(event='provenance',trace_mode='retail_plus',exe_sha256=t.config.EXE_HASH,
                     diagnostic_version=7,lifecycle_version=2,pid=10,created_filetime=20,images=images),
                dict(event='armed',phase='retail_plus',probes={k:hex(n) for n,k in enumerate(t.INITIAL)})]

    def test_release_changed_sources_and_stale_trace(self):
        rows=self.rows();data=lambda:b''.join(json.dumps(r).encode()+b'\n' for r in rows)
        cli.validate_release(data()+b'{partial',10,20)
        with self.assertRaises(DecodeError):cli.validate_release(data(),10,21)
        rows[0]['images']['observer']['sources']['retail_plus_trace_1360.py']='changed'
        with self.assertRaisesRegex(DecodeError,'plus_release_sources'):cli.validate_release(data(),10,20)

    def test_analysis_distinguishes_cleanup_and_budget(self):
        rows=self.rows()+[dict(event='plus_request',request=1,generation='one',user=0x10010029),
                          dict(event='plus_settlement',request=1,generation='one',user=0x10010029,side='resolve'),
                          dict(event='end',reason='deadline',cleanup_verified=True,decode_unknowns=0,
                               cleanup={'detached':True},cleanup_errors=[])]
        with tempfile.TemporaryDirectory(dir=Path(tempfile.gettempdir())/'opencode') as tmp:
            p=Path(tmp)/'trace.jsonl'
            def analyze():p.write_text(''.join(json.dumps(r)+'\n' for r in rows));return cli.analyze(p)
            self.assertEqual(analyze()['boundary_result'],'native_settlement_without_observed_executor_delivery')
            rows[-1]['reason']='hit_budget'
            result=analyze();self.assertTrue(result['cleanup_verified']);self.assertTrue(result['budget_exhausted'])
            self.assertEqual(result['boundary_result'],'incomplete_observation')
            self.assertFalse(result['focused_boundary_complete'])

    def test_wrong_profile_not_accepted(self):
        p=self.rows()[0];p['images']['plus']['sha256']='other_firmware'
        with self.assertRaisesRegex(DecodeError,'plus_pins'):cli.validate_profile(p)

    def test_startup_exceptions_are_not_decode_or_cleanup_errors(self):
        rows=self.rows()+[dict(event='forwarded_exception',code='0xc0000005',first=True),
            dict(event='plus_request',request=1,generation='one',user=0x10010029,resolve_id=100,reject_id=101),
            dict(event='plus_settlement',request=1,generation='one',user=0x10010029,side='resolve'),
            dict(event='plus_executor_callback',request=1,generation='one',user=0x10010029,side='resolve',callback_id=100),
            dict(event='plus_executor_return',request=1,generation='one',user=0x10010029),
            dict(event='end',reason='focused_boundary_complete',cleanup_verified=True,decode_unknowns=0,
                 cleanup={'detached':True},cleanup_errors=[])]
        with tempfile.TemporaryDirectory(dir=Path(tempfile.gettempdir())/'opencode') as tmp:
            p=Path(tmp)/'trace.jsonl'
            def analyze():p.write_text(''.join(json.dumps(r)+'\n' for r in rows));return cli.analyze(p)
            result=analyze()
            self.assertTrue(result['focused_boundary_complete']);self.assertEqual(result['forwarded_exceptions'],1)
            rows.insert(-1,dict(event='forwarded_exception',code='0xc0000005',first=True))
            result=analyze();self.assertTrue(result['decode_valid']);self.assertTrue(result['cleanup_verified'])
            self.assertTrue(result['exception_scope_uncertain']);self.assertFalse(result['focused_boundary_complete'])


if __name__ == '__main__':unittest.main()
