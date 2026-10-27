# Split-screen windows (ps2xRuntime/src/lib/host/split_windows.cpp: each player of a two-player match in a window of
# their own). Idempotent.
#  - Hud::Update (0x2814e0), before each element's update call: ps2x_split_hud_owner -- whose element it is (0x28156c
#    shared, 0x28160c player 1, 0x281644 player 2; -1 after the loop). For a player's element the hook hands it the
#    whole screen (f13..f16 = x0, x1, y0, y1) and the one-player screen config (a2 = 0) instead of its half.
#  - GameRenderer::RenderOverlays (0x22ac40) entry: owner 2 (overlays and the pause menu are seen by both players),
#    PcplSpriteBank::FlushSprites (0x262190) entry: owner -1.
#  - PcplSpriteRenderer::AddSprite (0x261e90) entry: ps2x_split_sprite moves the sprite's corners (a1..t0: Point2d
#    x | y << 32) into its owner's half, or adds a shared sprite twice (one copy per half) by running the function
#    once more from its entry with the first copy's corners.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401

TAG = 'patch_splitwin'
OWNER = 'extern "C" int ps2x_split_hud_owner(int owner);'


def patch(name, edits, decls):
    p = out(name)
    s = open(p, encoding='utf-8').read()
    if TAG in s:
        print('already patched', name)
        return
    for anchor, hook in edits:
        assert s.count(anchor) == 1, 'anchor %r in %s' % (anchor, name)
        s = s.replace(anchor, hook + ' // ' + TAG + '\n' + anchor, 1)
    assert s.count('#include "ps2_stubs.h"\n') == 1
    s = s.replace('#include "ps2_stubs.h"\n', '#include "ps2_stubs.h"\n' + ''.join(d + '\n' for d in decls), 1)
    open(p, 'w', encoding='utf-8', newline='').write(s)
    print('patched', name)


def call_site(pc, ret):
    return '        if (!runtime->dispatchGuestBranch(rdram, ctx, jumpTarget, 0x%Xu, 0x%Xu,' % (pc, ret)


FULL = ('        if (ps2x_split_hud_owner(%d)) { ctx->f[13] = 0.0f; ctx->f[14] = 1.0f; ctx->f[15] = 0.0f; '
        'ctx->f[16] = 1.0f; SET_GPR_U32(ctx, 6, 0u); }')
patch('Update__3HudFfPCv_0x2814e0.cpp',
      [(call_site(0x28156C, 0x281574), '        ps2x_split_hud_owner(2);'),
       (call_site(0x28160C, 0x281614), FULL % 0),
       (call_site(0x281644, 0x28164C), FULL % 1),
       ('label_281708:\n', '    ps2x_split_hud_owner(-1);')],
      [OWNER])

patch('RenderOverlays__12GameRendererFv_0x22ac40.cpp',
      [('    // 0x22ac40:', '    ps2x_split_hud_owner(2);')],
      [OWNER])
patch('FlushSprites__14PcplSpriteBankFUi_0x262190.cpp',
      [('    // 0x262190:', '    ps2x_split_hud_owner(-1);')],
      [OWNER])

FN = 'AddSprite__18PcplSpriteRendererF7Point2d7Point2d7Point2d7Point2dPC5BoundPA4_CUc_0x261e90'
SPRITE = '''    {
        uint64_t ps2xPts[4] = {GPR_U64(ctx, 5), GPR_U64(ctx, 6), GPR_U64(ctx, 7), GPR_U64(ctx, 8)};
        const int ps2xSplit = ps2x_split_sprite(rdram, GPR_U32(ctx, 4), ps2xPts, 0);
        if (ps2xSplit == 2)
        {
            // Shared sprite: the first copy goes through the whole function now, the second one below.
            const R5900Context ps2xEntry = *ctx;
            ps2x_split_sprite(rdram, GPR_U32(ctx, 4), ps2xPts, 1);
            SET_GPR_U64(ctx, 5, ps2xPts[0]); SET_GPR_U64(ctx, 6, ps2xPts[1]);
            SET_GPR_U64(ctx, 7, ps2xPts[2]); SET_GPR_U64(ctx, 8, ps2xPts[3]);
            %s(rdram, ctx, runtime);
            *ctx = ps2xEntry;
            ps2xPts[0] = GPR_U64(ctx, 5); ps2xPts[1] = GPR_U64(ctx, 6);
            ps2xPts[2] = GPR_U64(ctx, 7); ps2xPts[3] = GPR_U64(ctx, 8);
            ps2x_split_sprite(rdram, GPR_U32(ctx, 4), ps2xPts, 2);
        }
        if (ps2xSplit != 0)
        {
            SET_GPR_U64(ctx, 5, ps2xPts[0]); SET_GPR_U64(ctx, 6, ps2xPts[1]);
            SET_GPR_U64(ctx, 7, ps2xPts[2]); SET_GPR_U64(ctx, 8, ps2xPts[3]);
        }
    }''' % FN
patch(FN + '.cpp',
      [('    // 0x261e90:', SPRITE)],
      ['extern "C" int ps2x_split_sprite(uint8_t *rdram, uint32_t renderer, uint64_t *pts, int pass);'])

# DrawFace (0x237160): the scoreboard portraits are RwIm2D triangles in screen pixels (6 vertices, stride 0x40 at a1),
# not sprites; while the split is on they are drawn twice, squeezed into each half (ps2x_split_im2d).
FACE_CALL = '''    ctx->pc = 0x3258C0u;
    if (!runtime->callGuest(rdram, ctx, 0x3258C0u, 0x237348u, 0x237350u)) {'''
FACE = '''    if (ps2x_split_im2d(rdram, GPR_U32(ctx, 5), 6u, 0x40u, 0))
    {
        const R5900Context ps2xEntry = *ctx;
        ps2x_split_im2d(rdram, GPR_U32(ctx, 5), 6u, 0x40u, 1);
        ctx->pc = 0x3258C0u;
        if (!runtime->callGuest(rdram, ctx, 0x3258C0u, 0x237348u, 0x237350u)) {
            return;
        }
        *ctx = ps2xEntry;
        ps2x_split_im2d(rdram, GPR_U32(ctx, 5), 6u, 0x40u, 2);
    }'''
patch('DrawFace__FPfffiP8RwCameraP13RwSky2DVertexP8RwRaster_0x237160.cpp',
      [(FACE_CALL, FACE)],
      ['extern "C" int ps2x_split_im2d(uint8_t *rdram, uint32_t verts, uint32_t count, uint32_t stride, int pass);'])

# GameRenderer::RenderWorldCameras (0x229c00) entry: the world camera count (+0x2d0) keeps the split on while paused.
patch('RenderWorldCameras__12GameRendererFv_0x229c00.cpp',
      [('    // 0x229c00:', '    ps2x_split_world_cameras(READ32(ADD32(GPR_U32(ctx, 4), 720)));')],
      ['extern "C" void ps2x_split_world_cameras(uint32_t count);'])

# PcplSpriteBank::CreateTextureCamera (0x262960): its sprite is drawn into a texture (owner 3: untouched; the
# FlushSprites call right after it sets -1 again). Sprites added outside the HUD (owner -1) count as shared.
patch('CreateTextureCamera__14PcplSpriteBankFP9RwTextureiiP6RwRGBAiPP9RwTexture_0x262960.cpp',
      [('    if (!runtime->callGuest(rdram, ctx, 0x2613A0u, 0x262C60u, 0x262C68u)) {', '    ps2x_split_hud_owner(3);')],
      [OWNER])
