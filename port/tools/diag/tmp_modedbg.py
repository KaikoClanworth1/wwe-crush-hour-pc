# TEMP instrumentation: SkyStart video mode table + selected mode. Remove with --remove.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
import sys, re
path = out('SkyStart_0x34c1c0.cpp')
s = open(path, encoding='utf-8').read()
s = re.sub(r'    \{ // TEMP-MODEDBG.*?\n    \}\n', '', s, flags=re.S)
if '--remove' not in sys.argv:
    blk = ('    { // TEMP-MODEDBG\n'
           '        const uint32_t idx = READ32(ADD32(GPR_U32(ctx, 28), 0xFFFF86A0u));\n'
           '        std::printf("[modedbg] current mode=%u\\n", idx);\n'
           '        for (uint32_t m = 16; m < 72; ++m) { const uint32_t e = 0x469980u + m * 16u;\n'
           '            std::printf("[modedbg]  %2u: w=%d h=%d depth=%d flags=%08x\\n", m, (int)READ32(e), (int)READ32(e + 4), (int)READ32(e + 8), READ32(e + 12)); }\n'
           '        std::fflush(stdout);\n'
           '    }\n')
    a = '    // 0x34cfa4: 0x8f8386a0  lw          $v1, -0x7960($gp)\n'
    assert s.count(a) == 1
    s = s.replace(a, blk + a, 1)
open(path, 'w', encoding='utf-8', newline='').write(s)
print('done')
