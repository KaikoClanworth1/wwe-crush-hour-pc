# BLEZ/BGTZ/BLTZ/BGEZ (+likely/link forms) test the whole 64-bit register on the EE (MIPS III). The recompiler
# emitted GPR_S32 compares, so branches on genuine 64-bit values went the wrong way -- e.g. libgcc soft-float
# _fpadd_parts (1.0 - 3.0 = 6.0), which corrupted Havok MOPP trees. Fixed in control_flow_emitter.cpp; this
# rewrites already generated output in place.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
import glob, os, re

out = OUTPUT
pat = re.compile(r'(branch_taken_0x[0-9a-f]+ = \()GPR_S32(\(ctx, \d+\) (?:<|<=|>|>=) 0\))')
files = sites = 0
for p in glob.glob(os.path.join(out, '*.cpp')):
    s = open(p, encoding='utf-8').read()
    t, n = pat.subn(r'\1GPR_S64\2', s)
    if n:
        open(p, 'w', encoding='utf-8', newline='').write(t)
        files += 1
        sites += n
print(f'patched {sites} branch conditions in {files} files')
