import struct,sys
f=open('GameFiles/SLUS_203.85','rb').read()
e_shoff,=struct.unpack_from('<I',f,0x20); shnum,shstrndx=struct.unpack_from('<HH',f,0x30)
secs=[struct.unpack_from('<10I',f,e_shoff+i*40) for i in range(shnum)]
sym=secs[3]; st=secs[2]
out=[]
for i in range(sym[5]//16):
    n,v,sz,info,oth,shn=struct.unpack_from('<IIIBBH',f,sym[4]+i*16)
    name=f[st[4]+n:f.index(b'\0',st[4]+n)].decode('latin1')
    out.append((v,sz,info&15,info>>4,name))
out.sort()
with open('recomp/symbols.txt','w') as o:
    for v,sz,t,b,n in out: o.write(f"{v:08x} {sz:6x} {'FUNC' if t==2 else 'OBJ ' if t==1 else t} {n}\n")
