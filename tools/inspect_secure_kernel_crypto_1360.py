"""Static secure-kernel service inspection, PT_LOAD only; no firmware execution."""
from pathlib import Path
import contextlib
import hashlib
import re
import struct
import sys
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
root=Path(__file__).resolve().parents[1]
from local_paths import configured_path
fw=configured_path() / 'decrypted_modules'
p=fw/'secure_kernel_4.elf'; b=p.read_bytes()
phoff=struct.unpack_from('<I',b,28)[0]
entsize,count=struct.unpack_from('<HH',b,42)
segments=[struct.unpack_from('<8I',b,phoff+i*entsize) for i in range(count)]
def data(va,n):
    s=next(s for s in segments if s[0]==1 and s[2]<=va and va+n<=s[2]+s[4])
    off=s[1]+va-s[2]; assert off+n<=len(b)
    return b[off:off+n]
def word(va): return struct.unpack('<I',data(va,4))[0]
out=root/('_TempData/loginmgr-agent-sk-crypto-'+sys.argv[1]+'.txt')
with out.open('w',encoding='utf-8') as f,contextlib.redirect_stdout(f):
    print(p,len(b),hashlib.sha256(b).hexdigest(),'PT_LOAD',segments)
    if sys.argv[1]=='strings':
        for m in re.finditer(rb'[ -~]{6,160}',b):
            if any(s in m.group().lower() for s in [b'crypt',b'key',b'ccp',b'syscall',b'svc',b'aes']):
                s=next((s for s in segments if s[1]<=m.start()<s[1]+s[4]),None)
                print(hex(m.start()),hex(s[2]+m.start()-s[1]) if s else '-',m.group().decode())
    elif sys.argv[1]=='pointers':
        targets={int(x,0) for x in sys.argv[2:]}
        for s in segments:
            for va in range(s[2],s[2]+s[4]-3,4):
                if word(va) in targets: print('pointer',hex(va),hex(word(va)))
    else:
        for spec in sys.argv[2:]:
            parts=spec.split(':'); va,n=[int(x,0) for x in parts[:2]]
            if len(parts)>2 and parts[2]=='words':
                for at in range(va,va+n,4): print(hex(at),hex(word(at)))
                continue
            thumb=len(parts)<3 or parts[2]!='arm'
            cs=Cs(CS_ARCH_ARM,CS_MODE_THUMB if thumb else CS_MODE_ARM);cs.detail=True;cs.skipdata=True
            print('REGION',hex(va),hex(n),'Thumb' if thumb else 'ARM')
            for i in cs.disasm(data(va,n),va):
                notes=[]
                if i.id:
                    for op in i.operands:
                        if op.type==3 and i.reg_name(op.mem.base)=='pc':
                            at=(((i.address+4)&~3) if thumb else i.address+8)+op.mem.disp
                            try: notes.append(f'literal@{at:x}={word(at):08x}')
                            except (StopIteration,AssertionError): pass
                print(f'{i.address:08x}: {i.bytes.hex():10} {i.mnemonic:10} {i.op_str:48} {"; ".join(notes)}')
print(out)
