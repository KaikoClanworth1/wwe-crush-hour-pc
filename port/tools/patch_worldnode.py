# Generated code: the game's world renderer node (PCPL_WPipeNodeBody, 0x2b57f0) first offers the whole visible
# world to the native RenderWare path (nrw::pcplWorldNode in native_rw.cpp). When it draws every visible sector there
# (markers in RenderWare's open VIF packet), the node returns TRUE and RenderWare builds no world packets this frame.
# PS2X_NRW_WORLDNODE selects the mode. Idempotent.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
path = out('PCPL_WPipeNodeBody__FP14RxPipelineNodePC19RxPipelineNodeParam_0x2b57f0.cpp')
s = open(path, encoding='utf-8').read()
if 'patch_worldnode' in s:
    print('already patched')
else:
    entry = 'label_2b57f0:\n'
    assert s.count(entry) == 1, 'entry label not found'
    hook = ('label_2b57f0:\n'
            '    { // patch_worldnode: native world first\n'
            '        if (nrw::pcplWorldNode(rdram, runtime->memory(), GPR_U32(ctx, 4), GPR_U32(ctx, 5)))\n'
            '        {\n'
            '            SET_GPR_U64(ctx, 2, 1u); // TRUE\n'
            '            ctx->pc = GPR_U32(ctx, 31);\n'
            '            return;\n'
            '        }\n'
            '    }\n')
    s = s.replace(entry, hook, 1)
    decl = ('#include "ps2_stubs.h"\n'
            '#include "runtime/native_rw.h"\n'
            'namespace nrw\n{\n'
            '    bool pcplWorldNode(uint8_t *ee, PS2Memory &memory, uint32_t self, uint32_t params); // native_rw.cpp\n'
            '}\n')
    assert s.count('#include "ps2_stubs.h"\n') == 1
    s = s.replace('#include "ps2_stubs.h"\n', decl, 1)
    open(path, 'w', encoding='utf-8', newline='').write(s)
    print('patched')

