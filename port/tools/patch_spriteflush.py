# Generated code: PcplSpriteRenderer::Render (0x262590) sets the bank texture with
# RwRenderStateSet(rwRENDERSTATETEXTURERASTER, raster) and, if that fails, returns WITHOUT resetting the
# renderer's sprite count/cursor. In matches RenderWare's per-frame texture-upload ring fills up, the
# HUD sheet's set fails every frame, HUD sprites pile up past the renderer's 1024-sprite buffer and
# overwrite neighbouring heap objects (a player object's vtable -> crash in
# PowerupMessageHudElement::Update). Take the reset path instead: the sprites are dropped for that
# frame rather than accumulating.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
path = out('Render__18PcplSpriteRendererFR11RwMatrixTag_0x262590.cpp')
s = open(path, encoding='utf-8').read()
old = '''        if (branch_taken_0x2625b8) {
            ctx->pc = 0x262618u;
            goto label_262618;
        }'''
new = '''        if (branch_taken_0x2625b8) { // patch_spriteflush: still reset the sprite buffer
            ctx->pc = 0x262604u;
            goto label_262604;
        }'''
if 'patch_spriteflush' in s:
    print('already patched')
else:
    assert s.count(old) == 1 and 'label_262604:' in s
    s = s.replace(old, new, 1)
    open(path, 'w', encoding='utf-8', newline='').write(s)
    print('patched')
