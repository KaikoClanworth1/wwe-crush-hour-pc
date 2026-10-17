# TEMP instrumentation: log call counts + sample callers for RW resource/instancing functions.
# Remove with: python tmp_instcount.py --remove
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
import sys, re, os
out = OUTPUT
funcs = {
    'RwResourcesFreeResEntry_0x329300.cpp': '329300',
    'RwResourcesAllocateResEntry_0x3294f0.cpp': '3294f0',
    'RwResourcesEmptyArena_0x329810.cpp': '329810',
    'RpAtomicPS2AllInstance_0x2fe770.cpp': '2fe770',
}
remove = '--remove' in sys.argv
for fn, a in funcs.items():
    p = os.path.join(out, fn)
    s = open(p, encoding='utf-8').read()
    s = re.sub(r'    \{ // TEMP-INSTDBG.*?\n    \}\n', '', s, flags=re.S)
    if not remove:
        block = ('    { // TEMP-INSTDBG\n'
                 '        static uint64_t n = 0; ++n;\n'
                 '        if ((n & 511u) == 1u) { std::printf("[instdbg] %s n=%llu ra=%08x a0=%08x a1=%08x\\n", "' + fn.split('_0x')[0] + '", (unsigned long long)n, GPR_U32(ctx, 31), GPR_U32(ctx, 4), GPR_U32(ctx, 5)); '
                 'const uint32_t g = 0x4B8C50u + READ32(ADD32(GPR_U32(ctx, 28), 0xFFFF84C0u)); '
                 'std::printf("    a2=%08x arena size=%08x w1=%08x w2=%08x w9=%08x\\n", GPR_U32(ctx, 6), READ32(g), READ32(g + 4), READ32(g + 8), READ32(g + 0x24)); std::fflush(stdout); }\n'
                 '    }\n')
        lab = '    ctx->pc = 0x%su;\n\n' % a
        assert s.count(lab) == 1, fn
        s = s.replace(lab, lab + block, 1)
    open(p, 'w', encoding='utf-8', newline='').write(s)
    print(fn, 'removed' if remove else 'instrumented')
