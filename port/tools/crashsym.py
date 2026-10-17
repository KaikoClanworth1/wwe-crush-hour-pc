# Resolve "[crash]   #NN ps2EntryRunner.exe+0xOFFSET" lines (win32_host.cpp crash filter) with the linker map.
# Usage: python tools\crashsym.py [stdout.txt]
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps2x_paths import REPO, OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
import bisect, re, sys

mapf = os.path.join(REPO, 'out', 'build', 'ps2xRuntime', 'Release', 'ps2EntryRunner.map')
log = sys.argv[1] if len(sys.argv) > 1 else root('testrun', 'stdout.txt')

base = None
syms = []
for line in open(mapf, encoding='utf-8', errors='replace'):
    if base is None:
        m = re.search(r'Preferred load address is ([0-9a-fA-F]+)', line)
        if m:
            base = int(m.group(1), 16)
        continue
    m = re.match(r'\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s', line)
    if m:
        syms.append((int(m.group(2), 16) - base, m.group(1)))
syms.sort()
addrs = [a for a, _ in syms]

for line in open(log, encoding='utf-8', errors='replace'):
    if not line.startswith('[crash]'):
        continue
    m = re.search(r'(\S+)\+0x([0-9a-f]+)', line)
    if m and m.group(1).lower().startswith('ps2entryrunner'):
        off = int(m.group(2), 16)
        i = bisect.bisect_right(addrs, off) - 1
        name = syms[i][1] if i >= 0 else '?'
        print(line.rstrip(), ' ', name, '+0x%x' % (off - addrs[i] if i >= 0 else 0))
    else:
        print(line.rstrip())
