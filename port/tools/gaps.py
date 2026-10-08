import struct
f=open('GameFiles/SLUS_203.85','rb').read()
syms=[l.split(None,3) for l in open('recomp/symbols.txt')]
funcs=sorted((int(a,16),int(s,16),n.strip()) for a,s,t,n in (x if len(x)==4 else x+[''] for x in syms) if t=='FUNC' and int(s,16)>0)
skip={'__gnu_compiled_c','gcc2_compiled.','__gnu_compiled_cplusplus'}
notype={}
for x in syms:
    if len(x)==4 and x[2]=='0' and x[3].strip() not in skip: notype.setdefault(int(x[0],16),x[3].strip())
def word(a): return struct.unpack_from('<I',f,0x80+a-0x100000)[0]
gaps=[]
prev=0x100000
for a,s,n in funcs:
    if a<0x3d10e0 and a>prev:
        gaps.append((prev,a))
    prev=max(prev,a+s)
tot=0
for s,e in gaps:
    nz=sum(1 for a in range(s,e,4) if word(a)!=0)
    if nz>2:
        names=[notype[a] for a in notype if s<=a<e]
        tot+=1
        print(f"{s:08x}-{e:08x} {e-s:6x} nz={nz} {names[:4]}")
print(tot)
