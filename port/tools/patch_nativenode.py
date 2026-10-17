# Generated code: RenderWare's PS2All node body (PS2AllNodeBody, 0x2fbf60) first offers the object to the
# native RenderWare path (nrw::nodeBody, runtime/native_rw.h). When it is handled there -- constants computed
# from the RW structures, one marker per mesh appended to RW's open VIF packet -- RenderWare's PS2 pipeline
# (object setup, instancing, mesh bridges, texture cache, VU1 uploads) does not run at all.
# PS2X_NRW_NODE=0 keeps RenderWare's pipeline. Idempotent.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
path = out('PS2AllNodeBody_0x2fbf60.cpp')
s = open(path, encoding='utf-8').read()

if 'patch_nativenode' not in s:
    entry = 'label_2fbf60:\n'
    assert s.count(entry) == 1, 'entry label not found'
    hook = ('label_2fbf60:\n'
            '    { // patch_nativenode: native RenderWare path first\n'
            '        if (nrw::nodeBody(rdram, runtime->memory(), GPR_U32(ctx, 4), GPR_U32(ctx, 5)))\n'
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
    print('patched')
else:
    print('already patched')

# RpPDS_G3x_ADL_LightingUpload(ambient, directional): remember the lights PDS G3x objects are lit with, so the
# native path can light G3x atomics (RenderWare-generic math with these two lights).
path = out('RpPDS_G3x_ADL_LightingUpload_0x2dd1f0.cpp')
s = open(path, encoding='utf-8').read()
if 'patch_nativenode' not in s:
    entry = '        default: break;\n    }\n\n    ctx->pc = 0x2dd1f0u;\n'
    assert s.count(entry) == 1, 'ADL entry not found'
    s = s.replace(entry, '        default: break;\n    }\n\n'
                  '    nrw::g3xLights(GPR_U32(ctx, 4), GPR_U32(ctx, 5)); // patch_nativenode\n'
                  '    ctx->pc = 0x2dd1f0u;\n', 1)
    if '#include "runtime/native_rw.h"' not in s:
        s = s.replace('#include "ps2_stubs.h"', '#include "ps2_stubs.h"\n#include "runtime/native_rw.h"', 1)
    open(path, 'w', encoding='utf-8', newline='').write(s)
    print('patched ADL upload')
else:
    print('ADL upload already patched')
