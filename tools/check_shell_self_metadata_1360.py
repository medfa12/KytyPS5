"""Verify 13.60 SELF metadata in memory; optional argument: alternate.

Requires cryptography and the locally retained source/key dictionary. Writes
text evidence only; reports payloads only after same-tree signature verification.
"""
from pathlib import Path
import contextlib
import hashlib
import json
import struct
import sys
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.asymmetric import rsa, padding
from cryptography.hazmat.primitives import hashes
from cryptography.exceptions import InvalidSignature

root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'tools'))
from inspect_shell_auth_images_1360 import Image
from local_paths import configured_path
dictionary=configured_path(env='KYTY_KEY_DICTIONARY')
alternate=len(sys.argv)>1 and sys.argv[1]=='alternate'
fw=configured_path(env='KYTY_ALTERNATE_FIRMWARE_ROOT' if alternate else 'KYTY_FIRMWARE_ROOT')
sha=lambda b:hashlib.sha256(b).hexdigest()
out=root/('_TempData/loginmgr-agent-self-recovery'+('-alternate' if alternate else '')+'.txt')
with out.open('w',encoding='utf-8') as f,contextlib.redirect_stdout(f):
    print('SOURCE TREE', 'alternate' if alternate else 'active', fw)
    print('Dictionary provenance',dictionary,sha(dictionary.read_bytes()))
    kd=json.loads(dictionary.read_text())
    sm=(fw/'decrypted_modules/sm_authmgr.elf').read_bytes()
    print('SM SHA256',sha(sm))
    phoff=struct.unpack_from('<I',sm,28)[0]
    phentsize,phcount=struct.unpack_from('<HH',sm,42)
    # PT_LOAD mapping only; the advertised section table is beyond EOF.
    def smdata(va,size):
        for at in range(phoff,phoff+phcount*phentsize,phentsize):
            typ,off,base,_,extent,*_=struct.unpack_from('<8I',sm,at)
            if typ==1 and base<=va and va+size<=base+extent:
                assert off+extent<=len(sm)
                return sm[off+va-base:off+va-base+size]
        raise ValueError('SM address outside a file-backed load segment')
    key=bytes.fromhex(next(x['key'] for x in kd['ccp_keys'] if x['id']==41))
    iv=smdata(0x62d0,16)
    print('dictionary IV equals selected tree IV',iv==bytes.fromhex(kd['cf_keys']['x86_system']['iv']))
    assert key==bytes.fromhex(kd['cf_keys']['x86_system']['key'])
    modptr,modsize,eptr,esize=struct.unpack('<4I',smdata(0x8024,16))
    pub=rsa.RSAPublicNumbers(int.from_bytes(smdata(eptr,esize),'big'),int.from_bytes(smdata(modptr,modsize),'big')).public_key()
    print('ARM-selected CCP slot41 key candidate from dictionary; IV from current SM VA62d0; pubdesc VA8024')
    print('public modulus bytes',modsize,'SHA256',sha(smdata(modptr,modsize)),'exponent',pub.public_numbers().e)
    print('cf key descriptors',[(k,v['id'],len(v['key'])//2,len(v['iv'])//2) for k,v in kd['cf_keys'].items()])
    for volume,parts in [('system_b',['vsh','SceShellCore.elf']),('system_ex_b',['app','NPXS40087','eboot.bin'])]:
        image=Image(fw/'segments'/('dev_ssd0.'+volume))
        try: b=image.target(parts)
        finally: image.file.close()
        active=(fw/volume/Path(*parts)).read_bytes()
        hs,ms=struct.unpack_from('<HH',b,12)
        n=struct.unpack_from('<H',b,24)[0]&0xfff
        elf=0x20+n*0x20
        assert b[elf:elf+64]==active[:64]
        assert hs+ms<=len(b) and ms%16==0 and modsize==0x180
        print('same-tree ELF SHA256',sha(active),'header equal',b[elf:elf+64]==active[:64])
        print('embedded ELF header',b[elf:elf+64].hex())
        print('active ELF header',active[:64].hex())
        phoff=struct.unpack_from('<Q',b,elf+32)[0]
        phsize,phnum=struct.unpack_from('<HH',b,elf+54)
        info=(elf+0x4f+phnum*0x38)&~15
        activeinfo=struct.unpack_from('<Q',active,32)[0]+struct.unpack_from('<H',active,54)[0]*struct.unpack_from('<H',active,56)[0]
        assert b[info:info+64]==active[activeinfo:activeinfo+64]
        print('extended info exact match',hex(info),hex(activeinfo))
        print('PHT equal',b[elf+phoff:elf+phoff+phsize*phnum]==active[phoff:phoff+phsize*phnum])
        assert b[elf+phoff:elf+phoff+phsize*phnum]==active[phoff:phoff+phsize*phnum]
        for i in range(phnum):
            orig=struct.unpack_from('<II6Q',b,elf+phoff+i*phsize)
            cur=struct.unpack_from('<II6Q',active,phoff+i*phsize)
            print('PHDR',i,[hex(v) for v in orig],'active equal',orig==cur)
        print('SELF id',hex(struct.unpack_from('<I',b,8)[0]),'metadata ciphertext SHA256',sha(b[hs:hs+ms]))
        print('stored metadata zero bytes',b[hs:hs+ms].count(0),'of',ms)
        for i in range(n): print('SELF entry',i,[hex(v) for v in struct.unpack_from('<4Q',b,32+i*32)])
        cipher=Cipher(algorithms.AES(key),modes.CBC(iv)).decryptor()
        plain=cipher.update(b[hs:hs+ms])+cipher.finalize()
        message=b[:hs]+plain[:-0x180]
        signature=plain[-0x180:]
        try:
            pub.verify(b[hs+ms-0x180:hs+ms],b[:hs+ms-0x180],padding.PSS(mgf=padding.MGF1(hashes.SHA256()),salt_length=32),hashes.SHA256())
        except InvalidSignature: print('Untransformed stored metadata: signature FAIL')
        else: print('Untransformed stored metadata: signature PASS')
        print('candidate AES-CBC metadata SHA256',sha(plain),'signed message SHA256',sha(message))
        try:
            pub.verify(signature,message,padding.PSS(mgf=padding.MGF1(hashes.SHA256()),salt_length=32),hashes.SHA256())
        except InvalidSignature:
            assert not alternate, 'alternate positive control unexpectedly failed'
            print('FAIL: native-selected RSA-PSS-SHA256 verification; candidate plaintext is NOT an auth record')
        else:
            print('PASS: RSA-3072-PSS-SHA256, MGF1-SHA256, salt32 against current SM public key')
            for label,sig,msg in [
                ('header bit flip',signature,bytes([message[0]^1])+message[1:]),
                ('payload bit flip',signature,message[:hs+n*0x60]+bytes([message[hs+n*0x60]^1])+message[hs+n*0x60+1:]),
                ('signature bit flip',bytes([signature[0]^1])+signature[1:],message),
            ]:
                try: pub.verify(sig,msg,padding.PSS(mgf=padding.MGF1(hashes.SHA256()),salt_length=32),hashes.SHA256())
                except InvalidSignature: print('PASS: rejected',label)
                else: raise AssertionError('negative control accepted: '+label)
            payload=plain[n*0x60:-0x180]
            print('AUTHENTICATED metadata payload',payload.hex())
            print('AUTHENTICATED same-tree authority',hex(struct.unpack_from('<Q',b,info)[0]),'caps[0]',hex(int.from_bytes(payload[:8],'big')))
print(out)
