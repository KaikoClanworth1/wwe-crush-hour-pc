# TEMP instrumentation: dump RpWorldSector fields + first mesh. Remove with --remove.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
import sys, re
path = out('_rpSectorDefaultRenderCallBack_0x2f4160.cpp')
s = open(path, encoding='utf-8').read()
s = re.sub(r'    \{ // TEMP-SECTORDBG.*?\n    \}\n', '', s, flags=re.S)
if '--remove' not in sys.argv:
    blk = r'''    { // TEMP-SECTORDBG
        static int logged = 0;
        const uint32_t sec = GPR_U32(ctx, 4);
        if (logged < 6 && READ16(sec + 0x8C) > 50u) {
            ++logged;
            auto rf = [&](uint32_t a) { uint32_t b = READ32(a); float f; std::memcpy(&f, &b, 4); return f; };
            std::printf("[sectordbg] sec=%08x type=%d tris=%08x verts=%08x norms=%08x uv0=%08x uv1=%08x prelit=%08x res=%08x mesh=%08x pipe=%08x mlwb=%u nv=%u nt=%u\n",
                sec, (int)READ32(sec), READ32(sec + 4), READ32(sec + 8), READ32(sec + 0xC), READ32(sec + 0x10), READ32(sec + 0x14), READ32(sec + 0x30),
                READ32(sec + 0x34), READ32(sec + 0x80), READ32(sec + 0x84), READ16(sec + 0x88), READ16(sec + 0x8A), READ16(sec + 0x8C));
            const uint32_t v = READ32(sec + 8);
            if (v) std::printf("  v0=(%.2f %.2f %.2f) v1=(%.2f %.2f %.2f)\n", rf(v), rf(v + 4), rf(v + 8), rf(v + 12), rf(v + 16), rf(v + 20));
            const uint32_t uv = READ32(sec + 0x10);
            if (uv) std::printf("  uv0=(%.3f %.3f) uv1=(%.3f %.3f)\n", rf(uv), rf(uv + 4), rf(uv + 8), rf(uv + 12));
            const uint32_t pl = READ32(sec + 0x30);
            if (pl) std::printf("  prelit0=%08x prelit1=%08x\n", READ32(pl), READ32(pl + 4));
            const uint32_t t = READ32(sec + 4);
            if (t) std::printf("  tri0: %04x %04x %04x %04x  tri1: %04x %04x %04x %04x\n", READ16(t), READ16(t + 2), READ16(t + 4), READ16(t + 6), READ16(t + 8), READ16(t + 10), READ16(t + 12), READ16(t + 14));
            const uint32_t mh = READ32(sec + 0x80);
            if (mh) {
                std::printf("  meshhdr flags=%08x numMeshes=%u serial=%u totalIdx=%u firstOff=%08x\n", READ32(mh), READ16(mh + 4), READ16(mh + 6), READ32(mh + 8), READ32(mh + 12));
                for (uint32_t m = 0; m < 3 && m < READ16(mh + 4); ++m) {
                    const uint32_t me = mh + 16 + m * 12;
                    const uint32_t mat = READ32(me + 8);
                    std::printf("   mesh%u idx=%08x n=%u mat=%08x  mat.tex=%08x col=%08x pipe=%08x\n", m, READ32(me), READ32(me + 4), mat, mat ? READ32(mat) : 0u, mat ? READ32(mat + 4) : 0u, mat ? READ32(mat + 8) : 0u);
                    const uint32_t ix = READ32(me);
                    if (ix) std::printf("     idx: %u %u %u %u %u %u\n", READ16(ix), READ16(ix + 2), READ16(ix + 4), READ16(ix + 6), READ16(ix + 8), READ16(ix + 10));
                }
            }
            std::fflush(stdout);
        }
    }
'''
    a = '    ctx->pc = 0x2f4160u;\n\n'
    i = s.index(a)
    s = s[:i + len(a)] + blk + s[i + len(a):]
    if '#include <cstring>' not in s:
        s = s.replace('#include <stdexcept>', '#include <stdexcept>\n#include <cstring>', 1)
open(path, 'w', encoding='utf-8', newline='').write(s)
print('done')
