# TEMP instrumentation: bone matrices written by _rpSkinMatrixUpdating. Remove with --remove.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
import sys, re
path = out('_rpSkinMatrixUpdating_0x2dbea0.cpp')
s = open(path, encoding='utf-8').read()
s = re.sub(r'    \{ // TEMP-SKINDBG.*?\n    \}\n', '', s, flags=re.S)
if '--remove' not in sys.argv:
    def blk(tag, reg):
        return ('    { // TEMP-SKINDBG\n'
                '        static uint64_t n = 0; ++n;\n'
                '        if ((n % 2000u) < 3u) { float v[4][4]; for (int r = 0; r < 4; ++r) _mm_storeu_ps(v[r], ctx->vu0_vf[9 + r]);\n'
                '            std::printf("[skindbg] ' + tag + ' n=%llu dst=%08x ra=%08x [%.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f]\\n", (unsigned long long)n, GPR_U32(ctx, ' + str(reg) + '), GPR_U32(ctx, 31),\n'
                '                v[0][0], v[0][1], v[0][2], v[0][3], v[1][0], v[1][1], v[1][2], v[1][3], v[2][0], v[2][1], v[2][2], v[2][3], v[3][0], v[3][1], v[3][2], v[3][3]); std::fflush(stdout); }\n'
                '    }\n')
    for anchor, tag, reg in (('    // 0x2dbfbc: 0xf8890000  sqc2        $vf9, 0x0($a0)\n', 'A', 4),
                             ('    // 0x2dc030: 0xf8490000  sqc2        $vf9, 0x0($v0)\n', 'B', 2)):
        assert s.count(anchor) == 1, anchor
        s = s.replace(anchor, blk(tag, reg) + anchor, 1)
open(path, 'w', encoding='utf-8', newline='').write(s)
print('done')
