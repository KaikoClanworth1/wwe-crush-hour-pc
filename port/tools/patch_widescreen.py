# Widescreen: RwCameraSetViewWindow (0x330350) calls the host on entry, which widens the view window of the
# game's screen cameras to 16:9 when Widescreen=1 (ps2x_widescreen_view_window in ps2_runtime.cpp).
p = r'D:\Xbox Games Ports\WWE Crush Hour\recomp\output\RwCameraSetViewWindow_0x330350.cpp'
s = open(p, encoding='utf-8').read()
hook = '    ps2x_widescreen_view_window(rdram, GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 31));\n'
if hook not in s:
    anchor = '    ctx->pc = 0x330350u;\n\n    // 0x330350:'
    assert s.count(anchor) == 1
    s = s.replace(anchor, '    ctx->pc = 0x330350u;\n' + hook + '\n    // 0x330350:', 1)
    decl = 'extern "C" void ps2x_widescreen_view_window(uint8_t *rdram, uint32_t camera, uint32_t viewWindow, uint32_t ra);\n'
    s = s.replace('#include "ps2_stubs.h"\n', '#include "ps2_stubs.h"\n' + decl, 1)
    open(p, 'w', encoding='utf-8', newline='').write(s)

print('ok')
