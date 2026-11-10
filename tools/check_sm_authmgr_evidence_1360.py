"""Static 13.60 evidence checks only: no firmware or emulator execution."""
from pathlib import Path
import hashlib
import struct

root=Path(__file__).resolve().parents[1]
from local_paths import configured_path
fw=configured_path()
b=(fw/'decrypted_modules/sm_authmgr.elf').read_bytes()
assert hashlib.sha256(b).hexdigest()=='cfeabbcfec15255433eec7d6d90ad9e36da4c9288c55eb5c68a4ecb3a6c27711'
assert b==(fw/'secure_modules/sm_authmgr.elf').read_bytes()
phoff=struct.unpack_from('<I',b,28)[0]
entsize,count=struct.unpack_from('<HH',b,42)
segments=[struct.unpack_from('<8I',b,phoff+i*entsize) for i in range(count)]
assert all(s[0]==1 and s[1]+s[4]<=len(b) for s in segments)
assert struct.unpack_from('<I',b,32)[0]>len(b)
def data(va,n):
    s=next(s for s in segments if s[2]<=va and va+n<=s[2]+s[4])
    return b[s[1]+va-s[2]:s[1]+va-s[2]+n]

# Independently specified case destinations, checked against actual table bytes.
expected=[0x2190,0x21fe,0x220a,0x2218,0x21c2,0x221e,0x2224,0x222a,0x224e,0x21c8,0x2254,0x21ce]
assert [0x216c+2*v for v in data(0x2170,12)]==expected
assert data(0x7554,19)==bytes.fromhex('3031300d060960864801650304020105000420')

# Compare the observed REV+swapped-word transformation with an explicit vector.
src=bytes.fromhex('0123456789abcdef')
lo,hi=struct.unpack('<II',src)
rev=lambda v:int.from_bytes(v.to_bytes(4,'little'),'big')
assert struct.pack('<II',rev(hi),rev(lo))==bytes.fromhex('efcdab8967452301')

# Packed bitmap rank, checked against explicit expected record locations.
bitmap=bytes.fromhex('0501')+bytes(14)
rank=lambda j:sum((bitmap[i//8]>>(i%8))&1 for i in range(j))
assert [0x40+16*rank(j) for j in [0,2,8]]==[0x40,0x50,0x60]
assert rank(8)>=((0x60-0x30)//16-1)
assert rank(8)<((0x70-0x30)//16-1)

# Actual independent surviving container checks the location formula only.
u=(fw/'segments/updatemode.self').read_bytes()
hs,ms=struct.unpack_from('<HH',u,12)
n=struct.unpack_from('<H',u,24)[0]&0xfff
assert (hs,ms,n,(u[7]>>4)&7,(u[26]>>4)&7)==(0x2b0,0x3f0,6,3,5)
assert hs+n*0x60==0x4f0 and ms-n*0x60-0x180==0x30
lines=['PASS: actual ELF32 PT_LOAD bounds, identical SM copies, missing section table,',
       'actual classification TBB destinations, actual SHA-256 DigestInfo prefix,',
       'independent endian/bitmap vectors, actual updater metadata-location vector.',
       'These are static reconstruction checks, not native credential-success tests.']
(root/'_TempData/loginmgr-agent-arm-audit-checks.txt').write_text('\n'.join(lines)+'\n',encoding='utf-8')
print('\n'.join(lines))
