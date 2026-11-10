"""Read-only ELF32 ARM audit. Usage: label VA:size[:arm|words] ... ."""
from pathlib import Path
import contextlib
import hashlib
import struct
import sys
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB

root = Path(__file__).resolve().parents[1]
from local_paths import configured_path
path = configured_path() / 'decrypted_modules/sm_authmgr.elf'
b = path.read_bytes()
assert b[:6] == b'\x7fELF\x01\x01' and struct.unpack_from('<H',b,18)[0] == 40
phoff = struct.unpack_from('<I',b,28)[0]
entsize,count = struct.unpack_from('<HH',b,42)
segments = [struct.unpack_from('<8I',b,phoff+i*entsize) for i in range(count)]
segments = [s for s in segments if s[0] == 1]
def data(va,n):
    s = next(s for s in segments if s[2] <= va and va+n <= s[2]+s[4])
    o = s[1]+va-s[2]
    assert o+n <= len(b)
    return b[o:o+n]
def word(va): return struct.unpack('<I',data(va,4))[0]
def dis(va,n,thumb=True):
    decoder = Cs(CS_ARCH_ARM, CS_MODE_THUMB if thumb else CS_MODE_ARM)
    decoder.detail = True
    decoder.skipdata = True
    print('REGION',hex(va),hex(n),'THUMB' if thumb else 'ARM')
    for i in decoder.disasm(data(va,n),va):
        notes = []
        if i.id:
            for op in i.operands:
                if op.type == 3 and i.reg_name(op.mem.base) == 'pc':
                    at = ((i.address+4)&~3) + op.mem.disp if thumb else i.address+8+op.mem.disp
                    try: notes.append(f'literal@{at:x}={word(at):08x}')
                    except (StopIteration,AssertionError): pass
        print(f'{i.address:06x}: {i.bytes.hex():10} {i.mnemonic:10} {i.op_str:45} {"; ".join(notes)}')
out = root / ('_TempData/loginmgr-agent-arm-auth-' + sys.argv[1] + '.txt')
with out.open('w',encoding='utf-8') as f, contextlib.redirect_stdout(f):
    print(path,len(b),hashlib.sha256(b).hexdigest())
    print('PT_LOAD',segments)
    for region in sys.argv[2:]:
        parts=region.split(':'); va,n=(int(x,0) for x in parts[:2])
        if len(parts)>2 and parts[2]=='words':
            for at in range(va,va+n,4): print(f'{at:06x}: {word(at):08x}')
        else: dis(va,n,len(parts)<3 or parts[2]!='arm')
print(out)
