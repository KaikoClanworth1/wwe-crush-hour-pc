# Generated code: RenderWare's Im2D primitives (SkyIm2DRenderPrimitive 0x355250, SkyIm2DRenderIndexedPrimitive
# 0x356090) first offer the call to the native renderer (nrw::im2d, runtime/native_rw.h). When it is drawn there,
# RenderWare's Im2D VU1 packet is not built at all. PS2X_NRW_IM2D selects the mode. Idempotent.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
files = [
    (out('SkyIm2DRenderPrimitive_0x355250.cpp'), 'label_355250:\n',
     'nrw::im2d(rdram, runtime->memory(), GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6), 0u, 0u)'),
    (out('SkyIm2DRenderIndexedPrimitive_0x356090.cpp'), 'label_356090:\n',
     'nrw::im2d(rdram, runtime->memory(), GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6), GPR_U32(ctx, 7), GPR_U32(ctx, 8))'),
]
for path, entry, call in files:
    s = open(path, encoding='utf-8').read()
    if 'patch_im2d' in s:
        print('already patched', path)
        continue
    assert s.count(entry) == 1, 'entry label not found: ' + path
    hook = (entry +
            '    { // patch_im2d: native Im2D first\n'
            '        if (' + call + ')\n'
            '        {\n'
            '            SET_GPR_U64(ctx, 2, 1u); // TRUE\n'
            '            ctx->pc = GPR_U32(ctx, 31);\n'
            '            return;\n'
            '        }\n'
            '    }\n')
    s = s.replace(entry, hook, 1)
    if '#include "runtime/native_rw.h"' not in s:
        s = s.replace('#include "ps2_stubs.h"', '#include "ps2_stubs.h"\n#include "runtime/native_rw.h"', 1)
    open(path, 'w', encoding='utf-8', newline='').write(s)
    print('patched', path)
