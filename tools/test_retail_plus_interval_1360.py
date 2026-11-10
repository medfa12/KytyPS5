"""Offline privacy, exact lifecycle-field and balanced stack-sampling controls."""
import ctypes as C
import json
import struct
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from capture_native_hang import Capture,MemoryInfo,SIZE
from home_gate_trace_1360 import DecodeError
from sample_retail_plus_interval_1360 import classify,filtered,control_state


class FilterTests(unittest.TestCase):
    def test_no_raw_registers_stack_or_noncode_values_are_emitted(self):
        sample=dict(tid=7,rip=0x1010,rsp=0x7000,context=b'AUTH_TOKEN_DO_NOT_EMIT',
                    stack=struct.pack('<QQ',0x1020,0xdeadbeef)+b'AUTH_SECRET_STRING')
        result=filtered(sample,[(0x1000,0x1100,'host.exe',0x1000)])
        self.assertEqual(result['pc'],dict(image='host.exe',rva='0x10'))
        self.assertEqual(result['stack_code_candidates'],[dict(stack_offset='0x0',image='host.exe',rva='0x20')])
        text=json.dumps(result)
        for secret in ('AUTH','deadbeef','3735928559','context','stack_base64'):
            self.assertNotIn(secret,text)

    def test_unknown_code_is_not_dereferenced(self):
        self.assertIsNone(classify(0xdeadbeef,[]))
        self.assertEqual(filtered(dict(tid=7,rip=0xdeadbeef,context=b'',stack=b''),[])['pc']['image'],
                         'unknown/JIT; not read')

    def test_overlap_fails_closed(self):
        with self.assertRaises(DecodeError):classify(5,[(1,10,'a',0),(4,12,'b',0)])


class SamplingTests(unittest.TestCase):
    def fake(self,rsp=0x7010,fail_context=False,prior=0):
        f=SimpleNamespace(pid=10,process=123,resumes=0,reads=0,closed=0)
        def owner(h):return 10
        f.k=SimpleNamespace(GetProcessIdOfThread=owner)
        f.open_thread=lambda *a:321
        f.suspend=lambda h:prior
        def resume(h):f.resumes+=1;return prior+1
        f.resume=resume
        def close(h):f.closed+=1
        f.close=close
        def context(h,p):
            if fail_context:return 0
            C.c_uint64.from_address(p+152).value=rsp
            C.c_uint64.from_address(p+248).value=0x1010
            return 1
        f.context=context
        def query(process,at,out,size):
            info=C.cast(out,C.POINTER(MemoryInfo)).contents
            info.base=0x7000;info.size=0x1000;info.state=0x1000;info.protect=4
            return C.sizeof(MemoryInfo)
        f.query=query
        def read(process,at,out,size,count):
            f.reads+=1;C.memset(out,0,size);C.cast(count,C.POINTER(SIZE)).contents.value=size
            return 1
        f.read=read
        return f

    def test_bound_rejection_never_reads_stack_and_always_resumes(self):
        f=self.fake(rsp=0x9000)
        result=Capture.sample(f,7,8192,stack_bounds=(0x7000,0x7100))
        self.assertIn('provenance bounds',result['error'])
        self.assertEqual((f.reads,f.resumes,f.closed),(0,1,1))

    def test_read_is_clipped_to_proven_stack(self):
        f=self.fake()
        result=Capture.sample(f,7,8192,stack_bounds=(0x7000,0x7100))
        self.assertEqual(result['stack_size'],0xf0)
        self.assertEqual((result['previous_suspend_count'],result['resume_previous_count']),(0,1))
        self.assertEqual((f.resumes,f.closed),(1,1))

    def test_existing_suspend_count_is_preserved(self):
        f=self.fake(prior=4)
        result=Capture.sample(f,7,8192,stack_bounds=(0x7000,0x7100))
        self.assertEqual((result['previous_suspend_count'],result['resume_previous_count']),(4,5))
        self.assertEqual(f.resumes,1)

    def test_failed_context_still_removes_own_increment(self):
        f=self.fake(fail_context=True)
        result=Capture.sample(f,7,8192,stack_bounds=(0x7000,0x7100))
        self.assertIn('GetThreadContext',result['error'])
        self.assertEqual((f.reads,f.resumes,f.closed),(0,1,1))

    def test_foreign_thread_is_not_suspended(self):
        f=self.fake()
        def owner(h):return 11
        f.k.GetProcessIdOfThread=owner
        def suspend(h):raise AssertionError('foreign thread must not be suspended')
        f.suspend=suspend
        result=Capture.sample(f,7,8192,stack_bounds=(0x7000,0x7100))
        self.assertIn('ID reused',result['error']);self.assertEqual(f.resumes,0)


class ControlTests(unittest.TestCase):
    def fixture(self):
        host,run,c,e,s=0x140000000,0xff0000,0xff1000,0xff2000,0xff3000
        data,reads={},[]
        def put(at,b):data[at]=b
        q=lambda at,v:put(at,struct.pack('<Q',v))
        i=lambda at,v:put(at,struct.pack('<i',v))
        instruction=bytes.fromhex('488b1d')+struct.pack('<i',0x22df630-0xa53090)
        put(host+0xa53089,instruction);q(host+0x22df630,run)
        i(run+0x2c,510);i(run+0x30,511)
        put(run+0x20,b'\0'*9);q(run,1);i(run+0x34,0);i(run+0x38,-2141913086)
        q(run+0x40,c);q(run+0x50,e);q(c,s);q(e+8,c);q(e+0x18,s)
        put(s+0x308,bytes((1,1,1,0,0)));i(s+0x2e0,60);q(s+0x2e8,1000)
        q(s+0x300,1);q(s+0x388,0);q(s+0x390,0)
        def read(at,n):
            reads.append((at,n));b=data[at];self.assertEqual(len(b),n);return b
        cap=SimpleNamespace(read_exact=read)
        pe=SimpleNamespace(get_data=lambda at,n:instruction)
        workers={25732:{'argument':run},19972:{'argument':run}}
        return host,run,s,data,reads,cap,pe,workers

    def test_only_allowlisted_control_fields_are_read(self):
        host,run,s,data,reads,cap,pe,workers=self.fixture()
        with patch('pefile.PE',return_value=pe):result=control_state(cap,host,'not_opened.exe',workers)
        self.assertEqual(result['retirement_error'],60)
        self.assertTrue(result['coordinator']['retiring']);self.assertFalse(result['runtime']['stop'])
        self.assertFalse(any(run+8<=at<run+0x20 for at,n in reads)) # never Owner/credentials
        self.assertFalse(any(s<=at<s+0x2e0 for at,n in reads)) # never hooks/payloads

    def test_selected_must_match_both_frozen_worker_arguments(self):
        host,run,s,data,reads,cap,pe,workers=self.fixture();workers[19972]['argument']+=8
        with patch('pefile.PE',return_value=pe),self.assertRaisesRegex(DecodeError,'selected_worker_identity'):
            control_state(cap,host,'not_opened.exe',workers)
        self.assertFalse(any(s<=at<s+0x400 for at,n in reads))

    def test_changed_endpoint_backlink_is_unknown(self):
        host,run,s,data,reads,cap,pe,workers=self.fixture();data[0xff2000+0x18]=struct.pack('<Q',s+8)
        with patch('pefile.PE',return_value=pe),self.assertRaisesRegex(DecodeError,'endpoint_backlinks'):
            control_state(cap,host,'not_opened.exe',workers)


if __name__=='__main__':unittest.main()
