"""Read-only 13.60 provenance inventory; archive members stream to memory only."""
from pathlib import Path
import contextlib
import hashlib
import struct
import subprocess
import argparse

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('firmware_roots', type=Path, nargs='+')
parser.add_argument('--archive', type=Path, required=True)
parser.add_argument('--seven-zip', default='7z', help='Executable path or name on PATH')
args = parser.parse_args()
trees = args.firmware_roots
targets = ['SceShellCore.elf', 'decrypted_modules/SceShellCore.elf',
           'system_b/vsh/SceShellCore.elf', 'system_ex_b/app/NPXS40087/eboot.bin']
magic = (bytes.fromhex('4f153d1d'), bytes.fromhex('5414f5ee'))
def describe(label,b):
    print(label, 'size', len(b), 'SHA256', hashlib.sha256(b).hexdigest(), 'prefix32', b[:32].hex())
    if b[:5] == b'\x7fELF\x02':
        phoff=struct.unpack_from('<Q',b,32)[0]
        entsize,count=struct.unpack_from('<HH',b,54)
        end=phoff+entsize*count
        first=min(struct.unpack_from('<Q',b,phoff+i*entsize+8)[0] for i in range(count)
                  if struct.unpack_from('<I',b,phoff+i*entsize)[0]==1)
        print('extended info',hex(end),b[end:end+64].hex())
        print('after info to first LOAD',hex(end+64),hex(first),'nonzero',sum(x!=0 for x in b[end+64:first]))
    elif b[:4] in magic:
        hs,ms=struct.unpack_from('<HH',b,12)
        n=struct.unpack_from('<H',b,24)[0]&0xfff
        sig=(b[7]>>4)&7; layout=(b[26]>>4)&7
        stride={2:0x50,3:0x60,5:0x60}.get(layout)
        trailer={1:0x100,3:0x180,4:0x180}.get(sig)
        print('SELF hs/ms/n/sig/layout',hex(hs),hex(ms),n,sig,layout)
        if stride is not None and trailer is not None:
            print('post-decryption payload offset/length',hex(hs+n*stride),hex(ms-n*stride-trailer))

out=root/'_TempData/loginmgr-agent-arm-input-provenance.txt'
with out.open('w',encoding='utf-8') as f, contextlib.redirect_stdout(f):
    for tree in trees:
        print('\nTREE',tree)
        for target in targets:
            p=tree/target
            if p.is_file(): describe(target,p.read_bytes())
        total=elfs=0; selfs=[]; errors=[]
        for p in tree.rglob('*'):
            if not p.is_file(): continue
            total+=1
            try:
                with p.open('rb') as g: prefix=g.read(4)
            except OSError as e:
                errors.append((str(p),str(e))); continue
            elfs+=prefix==b'\x7fELF'
            if prefix in magic: selfs.append(str(p.relative_to(tree)))
        print('prefix inventory files/ELFs',total,elfs,'SELF-magic files',selfs,'read errors',errors)
        if (tree/'segments/updatemode.self').is_file(): describe('updater', (tree/'segments/updatemode.self').read_bytes())
    archive=args.archive
    seven=args.seven_zip
    listing=subprocess.run([seven,'l','-slt',str(archive)],capture_output=True,check=True).stdout.decode('utf-8','replace')
    blocks=listing.replace('\r\n','\n').split('\n\n')
    selected=[]
    print('\nARCHIVE target entries',archive)
    for block in blocks:
        lines=block.splitlines()
        name=next((s[7:] for s in lines if s.startswith('Path = ')),None)
        if name and (name.endswith('SceShellCore.elf') or ('NPXS40087' in name and ('eboot' in name.lower() or 'self' in name.lower()))):
            print(block)
            if 'Folder = -' in block: selected.append(name)
    # One representative archive ShellCore copy; all matching ShellUI eboot entries.
    selected=[s for s in selected if s=='SceShellCore.elf' or 'NPXS40087' in s]
    for name in selected:
        member=subprocess.run([seven,'x','-so',str(archive),name],capture_output=True,check=True).stdout
        describe('archive:'+name,member)
print(out)
