# Resolve PS2X_PROFILE samples against the MSVC .map file of ps2EntryRunner.exe.
import sys, re, bisect, collections

prof, mapfile = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40

lines = open(prof).read().split('\n')
base = int(lines[0].split()[1], 16)
samples = [(int(a, 16), int(n)) for a, n in (l.split() for l in lines[1:] if l.strip() and not l.startswith('S '))]
stacks = [(int(l.split()[1]), [int(x, 16) for x in l.split()[2:]]) for l in lines[1:] if l.startswith('S ')]

syms = []
pref = 0x140000000
for l in open(mapfile, errors='replace'):
    m = re.match(r'\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s', l)
    if m:
        syms.append((int(m.group(2), 16), m.group(1)))
    m2 = re.search(r'Preferred load address is ([0-9a-f]+)', l)
    if m2:
        pref = int(m2.group(1), 16)
syms.sort()
addrs = [a for a, _ in syms]

agg = collections.Counter()
total = 0
for rip, n in samples:
    total += n
    ext = bool(rip & 0x8000000000000000)
    rip &= 0x7FFFFFFFFFFFFFFF
    if rip == 0x7FFFFFFFFFFFFFFF:
        agg['<external, unattributed>'] += n
        continue
    rva = rip - base + pref
    i = bisect.bisect_right(addrs, rva) - 1
    name = syms[i][1] if i >= 0 and rip >= base and rip - base < 0x40000000 else '<external>'
    agg[('[ext] ' if ext else '') + name] += n
print('total samples', total)
for name, n in agg.most_common(top):
    print(f'{100.0*n/total:6.2f}%  {name[:150]}')

def sym(rip):
    rva = rip - base + pref
    i = bisect.bisect_right(addrs, rva) - 1
    return syms[i][1] if i >= 0 else '?'

def short(name):
    m = re.match(r'\?([^@]+)@', name)
    return m.group(1) if m else name[:40]

if '--stacks' in sys.argv:
    filt = None
    for a in sys.argv:
        if a.startswith('--filter='):
            filt = a[9:]
    agg2 = collections.Counter()
    for n, frames in stacks:
        names = [short(sym(f)) for f in frames]
        if filt and not any(filt in x for x in names):
            continue
        agg2[' < '.join(names[:8])] += n
    print('--- stacks')
    for k, n in agg2.most_common(top):
        print(f'{100.0*n/total:6.2f}%  {k[:400]}')

if '--inclusive' in sys.argv:
    # Inclusive share per function (counted once per sampled stack). Stacks are depth-limited, so
    # deep callers are under-counted.
    inc = collections.Counter()
    stotal = 0
    for n, frames in stacks:
        stotal += n
        for name in set(short(sym(f)) for f in frames):
            inc[name] += n
    print('--- inclusive (of %d stack samples)' % stotal)
    for k, n in inc.most_common(top):
        print(f'{100.0*n/max(stotal,1):6.2f}%  {k[:120]}')
