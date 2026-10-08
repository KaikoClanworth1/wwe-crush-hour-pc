# Generated code: BC0F/BC0T were emitted as never taken ("(false)"). Evaluate the real COP0 DMA
# condition via ps2x_cpcond0(runtime) (ps2_runtime.cpp). Recompiler fixed as well.
import glob, re

pat = re.compile(r'(// 0x[0-9a-f]+: 0x([0-9a-f]+)\s+(bc0tl|bc0fl|bc0t|bc0f)\b[^\n]*\n(?:[^\n]*\n){0,3}?[^\n]*const bool branch_taken_0x[0-9a-f]+ = )\(false\);')
decl = 'extern "C" bool ps2x_cpcond0(PS2Runtime *runtime);'
files = sites = 0
for f in glob.glob(r'D:\Xbox Games Ports\WWE Crush Hour\recomp\output\*.cpp'):
    s = open(f, encoding='utf-8', errors='surrogateescape').read()
    if 'bc0' not in s:
        continue
    def fix(m):
        global sites
        sites += 1
        cond = 'ps2x_cpcond0(runtime)' if m.group(3).startswith('bc0t') else '!ps2x_cpcond0(runtime)'
        return m.group(1) + '(' + cond + ');'
    n = pat.sub(fix, s)
    if n != s:
        if decl not in n:
            n = n.replace('#include "ps2_stubs.h"', '#include "ps2_stubs.h"\n' + decl, 1)
        open(f, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(n)
        files += 1
print('files', files, 'sites', sites)
