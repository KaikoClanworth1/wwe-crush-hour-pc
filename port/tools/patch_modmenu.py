# In-game mod menu hooks (ps2xRuntime/src/lib/host/mod_menu.cpp). Idempotent.
#  - Shell::Update (0x1d7bd0) entry: ps2x_modmenu_shell(rdram, this) -- the main menu's state each shell frame.
#  - ps2_wait_for_vsync (0x1771a0) entry: ps2x_mods_frame(rdram) -- applies the mods once per game frame.
import glob

root = r'D:\Xbox Games Ports\WWE Crush Hour\recomp\output'


def patch(pattern, anchor, hook, decl, tag):
    files = glob.glob(root + '\\' + pattern)
    assert len(files) == 1, pattern
    p = files[0]
    s = open(p, encoding='utf-8').read()
    if tag in s:
        print('already patched', pattern)
        return
    assert s.count(anchor) == 1, 'anchor ' + pattern
    s = s.replace(anchor, anchor + hook, 1)
    assert s.count('#include "ps2_stubs.h"\n') == 1
    s = s.replace('#include "ps2_stubs.h"\n', '#include "ps2_stubs.h"\n' + decl, 1)
    open(p, 'w', encoding='utf-8', newline='').write(s)
    print('patched', pattern)


patch('Update__5ShellFv_0x1d7bd0.cpp',
      'label_1d7bd0:\n',
      '    ps2x_modmenu_shell(rdram, GPR_U32(ctx, 4)); // patch_modmenu\n',
      'extern "C" void ps2x_modmenu_shell(uint8_t *rdram, uint32_t shell);\n',
      'patch_modmenu')
patch('ps2_wait_for_vsync__Fv_0x1771a0.cpp',
      '    ctx->pc = 0x1771a0u;\n\n',
      '    ps2x_mods_frame(rdram); // patch_modmenu\n',
      'extern "C" void ps2x_mods_frame(uint8_t *rdram);\n',
      'patch_modmenu')
