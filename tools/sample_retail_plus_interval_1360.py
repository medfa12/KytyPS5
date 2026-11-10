"""Offline by default; bounded identity-checked worker sampling, no debugger.

Only frozen-log-owned worker stacks are read. Raw registers/stack buffers are
not persisted: emit executable-address candidates only, never heap arguments.
"""
import argparse
import ctypes as C
import json
from pathlib import Path
import re
import struct
import sys

sys.dont_write_bytecode = True
from home_gate_trace_1360 import Image, require, digest
from retail_home_config_trace_1360 import EXE_HASH, PINS
from retail_plus_trace_1360 import PLUS

FROZEN = {'plus-native.jsonl':'21ccecf3bce421495022abfc2f672bc1b66da4d5f5c47a23682cbf957d629e75',
          'host.log':'b6044758f8b4fc3bb14e36ddf2136d8c5fb2de5ba4462642c99bdc7dda3dd4a3',
          'guest.log':'6102dc9059ebfe47b2c623cdb395caabe56b5d17dc020db56f0cca1dc010be46'}


def frozen(root):
    for name,sha in FROZEN.items():
        require(digest((root/name).read_bytes())==sha,'interval_frozen_'+name)
    rows=[json.loads(l) for l in (root/'plus-native.jsonl').read_text().splitlines()]
    p=next(r for r in rows if r['event']=='provenance')
    require(p['pid']==17500 and p['created_filetime']==134387106776455686 and p['exe_sha256']==EXE_HASH,
            'interval_frozen_identity')
    calls=[r for r in rows if r['event']=='plus_service_call']
    require(len(calls)==1 and calls[0]['tid']==11128,'interval_worker_provenance')
    text=(root/'guest.log').read_text(encoding='utf-8',errors='replace')
    workers={}
    for tid in (11128,25732,19972):
        matches=re.findall(r'Pthread run begin: ([^\n]*?)os_thread_id = '+str(tid)+r', entry = (0x[\da-f]+), arg = (0x[\da-f]+), stack_addr = (0x[\da-f]+), stack_size = (\d+)',text)
        require(len(matches)==1,'interval_unique_thread_stack')
        name,entry,argument,base,size=matches[0];base,size=int(base,16),int(size)
        require(0x10000<=base and 0<size<=16*1024*1024,'interval_stack_bounds')
        workers[tid]=dict(role='Plus worker' if tid==11128 else 'NP transport' if tid==25732 else 'NP attach',
                          entry=int(entry,16),argument=int(argument,16),bounds=(base,base+size))
    return p,text,workers


def executable_ranges(modules,stage,text):
    import pefile
    ranges=[]
    for base,size,name in modules:
        p=Path(name)
        # Local PE metadata only; do not execute or download a symbol provider.
        pe=pefile.PE(str(p),fast_load=True)
        for section in pe.sections:
            if section.Characteristics & 0x20000000:
                ranges.append((base+section.VirtualAddress,base+min(size,section.VirtualAddress+section.Misc_VirtualSize),p.name,base))
    for key,(name,sha) in {**PINS,'plus':PLUS}.items():
        image=Image((stage/name).read_bytes(),sha)
        placements=re.findall(r'place '+re.escape(name)+r' base=(0x[\da-f]+) mapped=(0x[\da-f]+)',text)
        require(len(placements)==1,'interval_mapping_'+key)
        base,extent=(int(v,16) for v in placements[0])
        for s in image.loads:
            if s[1]&1:
                require(s[3]+s[5]<=extent,'interval_image_extent')
                ranges.append((base+s[3],base+s[3]+s[5],name,base))
    return ranges


def classify(value,ranges):
    matches=[dict(image=name,rva=hex(value-base)) for start,end,name,base in ranges if start<=value<end]
    require(len(matches)<=1,'interval_overlapping_code_ranges')
    return matches[0] if matches else None


def filtered(sample,ranges):
    row={k:sample[k] for k in ('tid','previous_suspend_count','resume_previous_count','stopped_ms','error','stack_size') if k in sample}
    if 'context' not in sample:return row
    row['pc']=classify(sample['rip'],ranges) or {'image':'unknown/JIT; not read'}
    row['stack_code_candidates']=[]
    for at in range(0,len(sample.get('stack',b''))-7,8):
        value=struct.unpack_from('<Q',sample['stack'],at)[0]
        code=classify(value,ranges)
        if code:
            row['stack_code_candidates'].append(dict(stack_offset=hex(at),**code))
            if len(row['stack_code_candidates'])>=64:break
    row['candidates_are_not_unwound_frames']=True
    return row


def control_state(cap,host,exe,workers):
    """Exact-options layout + actual IDA audit for theme-session4 ONLY.

    Reads no Owner, credential, IPMI input/output, request name or callback data.
    Selected must equal BOTH retained runtime workers' frozen launch argument.
    """
    import pefile
    pe=pefile.PE(str(exe),fast_load=True)
    instruction=pe.get_data(0xa53089,7)
    require(instruction[:3]==bytes.fromhex('488b1d') and
            cap.read_exact(host+0xa53089,7)==instruction,'interval_selected_abi')
    selected=host+0xa53090+struct.unpack('<i',instruction[3:])[0]
    q=lambda at:struct.unpack('<Q',cap.read_exact(at,8))[0]
    i=lambda at:struct.unpack('<i',cap.read_exact(at,4))[0]
    run=q(selected)
    require(run==workers[25732]['argument']==workers[19972]['argument'] and run!=0,'interval_selected_worker_identity')
    transport_tid,delivery_tid=i(run+0x2c),i(run+0x30)
    # Pthread's log unique_id is NOT guest.thread_id. The retained workers'
    # common argument + selected/backlinks establish the run, not guessed IDs.
    require(transport_tid>0 and delivery_tid>0 and transport_tid!=delivery_tid,'interval_runtime_thread_identity')
    flags=cap.read_exact(run+0x20,9)
    require(all(x in (0,1) for x in flags),'interval_runtime_flags')
    coordinator,endpoint=q(run+0x40),q(run+0x50)
    require(coordinator>=0x10000 and endpoint>=0x10000,'interval_coordinator_published')
    state=q(coordinator)
    require(state>=0x10000 and q(endpoint+8)==coordinator and q(endpoint+0x18)==state,'interval_coordinator_endpoint_backlinks')
    # Compiler fixture pins State retirement_request+2e0 and status+308..30c.
    state_flags=cap.read_exact(state+0x308,5)
    require(all(x in (0,1) for x in state_flags),'interval_coordinator_flags')
    result=dict(token=q(run),transport_guest_tid=transport_tid,delivery_guest_tid=delivery_tid,runtime=dict(zip(('starting','stop','owner_lost','attached','transport_returning',
        'delivery_returning','reaping','coordinator_done','delivery_detached'),map(bool,flags))),
        worker_error=i(run+0x34),attach_result=hex(i(run+0x38)&0xffffffff),
        coordinator=dict(zip(('registered','connecting','retiring','done','connected'),map(bool,state_flags))),
        retirement_error=i(state+0x2e0),timeout_ms=q(state+0x2e8),
        native_connection_retained=bool(q(state+0x300)),queued_jobs=q(state+0x388),active_job=bool(q(state+0x390)))
    require(q(selected)==run and q(run+0x40)==coordinator and q(coordinator)==state,'interval_control_state_changed')
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--checkpoint',type=Path,required=True)
    p.add_argument('--stage',type=Path,required=True)
    p.add_argument('--exe',type=Path,required=True)
    p.add_argument('--pid',type=int,default=17500)
    p.add_argument('--created-filetime',type=int,default=134387106776455686)
    p.add_argument('--exe-sha256',default=EXE_HASH)
    p.add_argument('--sample',action='store_true')
    p.add_argument('--controls',action='store_true',help='Explicit allowlisted native lifecycle flags; no heap payloads')
    p.add_argument('--unwind',action='store_true',help='Unwind captured stacks after resume using local-only image metadata')
    p.add_argument('--output',type=Path)
    args=p.parse_args()
    provenance,text,workers=frozen(args.checkpoint)
    require((args.pid,args.created_filetime,args.exe_sha256)==
            (provenance['pid'],provenance['created_filetime'],EXE_HASH),'interval_target_identity')
    require(digest(args.exe.read_bytes())==EXE_HASH,'interval_exe_pin')
    if not args.sample:
        print(json.dumps(dict(offline_verified=True,process_access=False,workers=workers),indent=2));return
    require(args.output is not None and not args.output.exists(),'interval_new_output')
    from capture_native_hang import Capture,bind,BOOL,HANDLE
    from capture_home_gates_1360 import process_identity
    symbols=str(args.exe.resolve().parent)
    require(not symbols.startswith('\\\\') and not any(c in symbols for c in (';','*')),'interval_local_symbols')
    cap=Capture(args.pid,symbols,False)
    rows=[]
    def identity():
        process_identity(cap,args)
        present=BOOL()
        require(bind(cap.k,'CheckRemoteDebuggerPresent',BOOL,HANDLE,C.POINTER(BOOL))(
            cap.process,C.byref(present)) and not present.value,'interval_debugger_present')
    try:
        cap.check_architecture();identity()
        modules=cap.modules()
        cap.module_list=modules
        if args.unwind:cap.init_symbols()
        hosts=[(b,n) for b,n,path in modules if Path(path).resolve()==args.exe.resolve()]
        require(len(hosts)==1,'interval_host_mapping')
        ranges=executable_ranges(modules,args.stage,text)
        current=set(cap.threads())
        for round_ in range(3):
            for tid,worker in workers.items():
                identity()
                if tid not in current:
                    rows.append(dict(round=round_,tid=tid,role=worker['role'],error='frozen worker no longer enumerated'));continue
                sample=cap.sample(tid,8192,stack_bounds=worker['bounds'])
                row=filtered(sample,ranges)
                if args.unwind and 'context' in sample:
                    row['unwound_frames']=[]
                    for pc,sp in cap.unwind(sample,32):
                        code=classify(pc,ranges)
                        if code is None:break
                        row['unwound_frames'].append(dict(stack_offset=hex(sp-sample['rsp']),**code))
                row.update(round=round_,role=worker['role'])
                if 'context' in sample:
                    require(worker['bounds'][0]<=sample['rsp']<worker['bounds'][1],'interval_live_stack_mismatch')
                rows.append(row)
        identity()
        report=dict(pid=args.pid,created_filetime=args.created_filetime,exe_sha256=EXE_HASH,
                    debugger_attached=False,raw_memory_persisted=False,maximum_stack_bytes_per_sample=8192,
                    frozen=FROZEN,samples=rows)
        if args.controls:
            identity()
            report['control_samples']=[control_state(cap,hosts[0][0],args.exe,workers) for _ in range(3)]
            identity()
        with args.output.open('x',encoding='utf-8') as out:json.dump(report,out,indent=2)
        print(json.dumps(report,indent=2))
    finally:cap.finish()


if __name__=='__main__':main()
