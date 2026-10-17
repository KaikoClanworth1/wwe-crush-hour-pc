# TEMP instrumentation: Gfx::Move(pos, quat) inputs. Remove with --remove.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
import sys, re, glob
path = glob.glob(out('Move__3GfxFRA4_fRA4_f_0x165b90.cpp'))[0]
s = open(path, encoding='utf-8').read()
s = re.sub(r'    \{ // TEMP-MOVEDBG.*?\n    \}\n', '', s, flags=re.S)
if '--remove' not in sys.argv:
    blk = ('    { // TEMP-MOVEDBG\n'
           '        static uint64_t n = 0; ++n;\n'
           '        auto rf = [&](uint32_t p) { uint32_t b = READ32(p); float f; std::memcpy(&f, &b, 4); return f; };\n'
           '        const uint32_t p = GPR_U32(ctx, 5), q = GPR_U32(ctx, 6);\n'
           '        const float qm = rf(q) * rf(q) + rf(q + 4) * rf(q + 4) + rf(q + 8) * rf(q + 8) + rf(q + 12) * rf(q + 12);\n'
           '        if ((n % 500u) < 6u || qm > 1.1f || qm < 0.9f) { static int lim = 0; if (lim++ < 400) {\n'
           '            std::printf("[movedbg] n=%llu this=%08x ra=%08x pos=(%.2f %.2f %.2f %.2f) q=(%.3f %.3f %.3f %.3f) |q|2=%.3f\\n", (unsigned long long)n, GPR_U32(ctx, 4), GPR_U32(ctx, 31),\n'
           '                rf(p), rf(p + 4), rf(p + 8), rf(p + 12), rf(q), rf(q + 4), rf(q + 8), rf(q + 12), qm); std::fflush(stdout); } }\n'
           '    }\n')
    a = '    ctx->pc = 0x165b90u;\n\n'
    assert s.count(a) == 1
    s = s.replace(a, a + blk, 1)
open(path, 'w', encoding='utf-8', newline='').write(s)
print('done')
