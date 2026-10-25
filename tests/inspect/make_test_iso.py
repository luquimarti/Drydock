# Writes a tiny synthetic Saturn disc (test.iso: IP.BIN header, a boot file, a
# subdirectory with an overlay) and matching work RAM dumps (race-*.bin), to test
# `drydock --inspect` and `--check`. No game data involved: everything is random.
#   python make_test_iso.py OUTDIR
import sys
import struct, os, random
random.seed(1)
out=sys.argv[1] if len(sys.argv) > 1 else '.'
os.makedirs(out, exist_ok=True)
S=2048
def both16(v): return struct.pack('<H',v)+struct.pack('>H',v)
def both32(v): return struct.pack('<I',v)+struct.pack('>I',v)
def rec(name,lba,size,isdir):
    n=name if isinstance(name,bytes) else name.encode()
    l=33+len(n); l+= l%2
    r=bytes([l,0])+both32(lba)+both32(size)+bytes(7)+bytes([2 if isdir else 0,0,0])+both16(1)+bytes([len(n)])+n
    return r+bytes(l-len(r))
boot=bytes(random.getrandbits(8) for _ in range(40000))
ovl=bytes(random.getrandbits(8) for _ in range(12000))
txt=b"hello\n"*10
# layout: root dir 20, DATA dir 21, files from 22
f_boot=22; f_txt=f_boot+(len(boot)+S-1)//S; f_ovl=f_txt+1
root=rec(b'\x00',20,S,True)+rec(b'\x01',20,S,True)+rec('0.BIN;1',f_boot,len(boot),False)+rec('DATA',21,S,True)+rec('README.TXT;1',f_txt,len(txt),False)
data=rec(b'\x00',21,S,True)+rec(b'\x01',20,S,True)+rec('OVL.PRG;1',f_ovl,len(ovl),False)
total=f_ovl+(len(ovl)+S-1)//S
img=bytearray(total*S)
ip=bytearray(b' '*256)
ip[0:16]=b'SEGA SEGASATURN '; ip[16:32]=b'SEGA TP T-999   '; ip[32:42]=b'T-TEST01  '; ip[42:48]=b'V1.000'
ip[48:56]=b'19960101'; ip[56:64]=b'CD-1/1  '; ip[64:74]=b'JUE       '; ip[80:96]=b'J               '
ip[96:208]=b'TEST TITLE'.ljust(112); ip[224:256]=struct.pack('>IIIIII',0x1000,0,0,0,0x06004000,0)+bytes(8)
img[0:256]=ip
pvd=bytearray(S); pvd[0]=1; pvd[1:6]=b'CD001'; pvd[6]=1; pvd[80:88]=both32(total); pvd[156:156+34]=rec(b'\x00',20,S,True)
img[16*S:17*S]=pvd
term=bytearray(S); term[0]=255; term[1:6]=b'CD001'; term[6]=1; img[17*S:18*S]=term
img[20*S:20*S+len(root)]=root; img[21*S:21*S+len(data)]=data
img[f_boot*S:f_boot*S+len(boot)]=boot; img[f_txt*S:f_txt*S+len(txt)]=txt; img[f_ovl*S:f_ovl*S+len(ovl)]=ovl
open(os.path.join(out,'test.iso'),'wb').write(img)
hw=bytearray(1<<20); hw[0x4000:0x4000+len(boot)]=boot; hw[0x80000:0x80000+len(ovl)]=ovl; hw[0x80000+100]^=0xFF
open(os.path.join(out,'race-hwram.bin'),'wb').write(hw); open(os.path.join(out,'race-lwram.bin'),'wb').write(bytes(1<<20))

