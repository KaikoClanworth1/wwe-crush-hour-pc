# Replace the EE libgcc soft-float double routines (fp-bit: fptodp, dptofp, dpadd, dpsub, dpmul, dpdiv, dpcmp,
# litodp, dptoli) with exact host IEEE double math. Doubles travel as raw 64-bit GPR values ($a0/$a1 -> $v0);
# fptodp takes $f12, dptofp returns $f0; "long" is 32-bit. fp-bit rounds to nearest and keeps denormals, so the
# host op runs under a clean MXCSR (the EE thread itself runs round-toward-zero + FTZ/DAZ).
# The recompiled versions built corrupt Havok MOPP trees (HARDCORE floor holes: 331/1258 triangles unreachable).
# PS2X_SOFTFLOAT_GUEST=1 falls back to the recompiled guest code.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
import glob, os

out = OUTPUT
TAG = '// patch_softfloat'

PRE = '''    {{ {tag}
        static const bool s_guest = std::getenv("PS2X_SOFTFLOAT_GUEST") != nullptr;
        if (!s_guest && ctx->pc == 0x{addr:x}u)
        {{
            auto argD = [&](int r) {{ const uint64_t b = static_cast<uint64_t>(GPR_U64(ctx, r)); double d; std::memcpy(&d, &b, 8); return d; }};
            auto retD = [&](double d) {{ uint64_t b; std::memcpy(&b, &d, 8); SET_GPR_U64(ctx, 2, b); }};
            (void)argD; (void)retD;
            const unsigned int savedCsr = _mm_getcsr();
            _mm_setcsr(0x1F80u);
{body}
            _mm_setcsr(savedCsr);
            ctx->pc = GPR_U32(ctx, 31);
            return;
        }}
    }}
'''

BODIES = {
    'fptodp': '            retD(static_cast<double>(ctx->f[12]));',
    'dptofp': '            ctx->f[0] = static_cast<float>(argD(4));',
    'dpadd': '            retD(argD(4) + argD(5));',
    'dpsub': '            retD(argD(4) - argD(5));',
    'dpmul': '            retD(argD(4) * argD(5));',
    'dpdiv': '            retD(argD(4) / argD(5));',
    'dpcmp': ('            const double a = argD(4), b = argD(5);\n'
              '            SET_GPR_S32(ctx, 2, (a != a || b != b) ? 1 : (a < b ? -1 : (a > b ? 1 : 0)));'),
    'litodp': '            retD(static_cast<double>(static_cast<int32_t>(GPR_U32(ctx, 4))));',
    'dptoli': ('            const double a = argD(4);\n'
               '            const int32_t v = (a != a) ? 0 : (a >= 2147483647.0 ? INT32_MAX : (a <= -2147483648.0 ? INT32_MIN : static_cast<int32_t>(a)));\n'
               '            SET_GPR_S32(ctx, 2, v);'),
}

for name, body in BODIES.items():
    files = glob.glob(os.path.join(out, f'{name}_0x*.cpp'))
    assert len(files) == 1, (name, files)
    p = files[0]
    s = open(p, encoding='utf-8').read()
    if TAG in s:
        print(name, 'already patched')
        continue
    addr = int(p.rsplit('_0x', 1)[1][:-4], 16)
    if '#include <immintrin.h>' not in s:
        s = s.replace('#include <stdexcept>\n', '#include <stdexcept>\n#include <immintrin.h>\n#include <cstring>\n', 1)
    i = s.index('    switch (ctx->pc) {')
    s = s[:i] + PRE.format(tag=TAG, addr=addr, body=body) + s[i:]
    open(p, 'w', encoding='utf-8', newline='').write(s)
    print('patched', name, hex(addr))
