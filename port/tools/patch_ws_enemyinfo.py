# Widescreen: EnemyInfoHudElement::Update (0x241660) places a car's name tag / health bar at the camera-space
# projection of the car (RwV3dTransformPoints with the camera's view matrix, then x/z, y/z: normalized screen
# 0..1), which follows the 16:9 view window of the 3D camera. The HUD layer is drawn 3/4 as wide about the frame's
# centre (gs_d3d11_backend.cpp), so once the on-screen checks have passed the position is spread by 4/3 about the
# centre (ps2x_ws_hud_x in ps2_runtime.cpp): the tag then lands over the car again. Idempotent.
import glob
paths = glob.glob(r'D:\Xbox Games Ports\WWE Crush Hour\recomp\output\Update__19EnemyInfoHudElementFfffffPCvQ218Pcpl2DScreenConfig12ScreenConfigi_0x241660.cpp')
assert len(paths) == 1
p = paths[0]
s = open(p, encoding='utf-8').read()
if 'patch_ws_enemyinfo' in s:
    print('already patched')
else:
    anchor = '    ctx->pc = 0x241A9Cu;\nlabel_241a9c:\n'
    assert s.count(anchor) == 1, 'anchor'
    hook = '    ps2x_ws_hud_x(rdram, ADD32(GPR_U32(ctx, 29), 0x1B0u)); // patch_ws_enemyinfo: tag x (normalized)\n'
    s = s.replace(anchor, hook + anchor, 1)
    decl = 'extern "C" void ps2x_ws_hud_x(uint8_t *rdram, uint32_t addr);\n'
    assert s.count('#include "ps2_stubs.h"\n') == 1
    s = s.replace('#include "ps2_stubs.h"\n', '#include "ps2_stubs.h"\n' + decl, 1)
    open(p, 'w', encoding='utf-8', newline='').write(s)
    print('patched')
