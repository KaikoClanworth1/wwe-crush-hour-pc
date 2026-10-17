# TEMP instrumentation: Wheel::PositionVisually final matrix + vehicle matrix. Remove with --remove.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
import sys, re, glob
path = glob.glob(out('PositionVisually__5Wheel*_0x1e1070.cpp'))[0]
s = open(path, encoding='utf-8').read()
s = re.sub(r'    \{ // TEMP-WHEELDBG.*?\n    \}\n', '', s, flags=re.S)
if '--remove' not in sys.argv:
    blk = ('    { // TEMP-WHEELDBG\n'
           '        static uint64_t n = 0; ++n;\n'
           '        if ((n % 240u) < 4u) {\n'
           '            auto mat = [&](const char *nm, uint32_t p) { std::printf(" %s[", nm); for (int k = 0; k < 16; ++k) { uint32_t b = READ32(p + 4u * k); float f; std::memcpy(&f, &b, 4); std::printf("%.2f%s", f, (k % 4 == 3) ? (k == 15 ? "" : " |") : " "); } std::printf("]"); };\n'
           '            std::printf("[wheeldbg] n=%llu node=%u", (unsigned long long)n, READ32(GPR_U32(ctx, 21) + 0x1C) & 0xFFFFu);\n'
           '            mat("final", GPR_U32(ctx, 29) + 0x140u); mat("veh", GPR_U32(ctx, 19)); std::printf("\\n"); std::fflush(stdout); }\n'
           '    }\n')
    a = '    // 0x1e13f4: 0x96a5001c  lhu         $a1, 0x1C($s5)\n'
    assert s.count(a) == 1
    s = s.replace(a, blk + a, 1)
open(path, 'w', encoding='utf-8', newline='').write(s)
print('done')
