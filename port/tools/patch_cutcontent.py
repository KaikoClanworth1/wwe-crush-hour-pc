# Hooks for the restored cut content (ps2xRuntime/src/lib/host/mod_menu.cpp: Ladder arena, Faarooq). Idempotent.
#  - Shell::OnInitLevelSelect_Tag (0x1d6520) entry: ps2x_levelsel_init -- a new arena chooser.
#  - Shell::OnLevelSelection_Tag (0x1d5760): the chooser slot from the script's LEVEL_SELECT (ps2x_levelsel_pick,
#    after atoi), and the unlocked-arena bits tested for it (ps2x_levelsel_unlock, after the lw at 0x1d57d0).
#  - Shell::OnSetGameLevel_Tag (0x1d6200): the picked chooser slot (ps2x_levelsel_pick, after atoi).
#  - InBetween_set_level_texture (0x169040) entry: the load screen for a level name (ps2x_level_loadscreen).
#  - VehicleLoader::LoadVehicle (0x233730) entry: the vehicle bundle for a superstar name (ps2x_vehicle_name).
#  - PlayMode_init_mode (0x19cf80), before Level::Init: the level file name (ps2x_level_file: the ladder arena has
#    no level_dm / level_tt / level_dd, only level.wg).
#  - LevelParseSemantics::Type (0x21b760) entry: a LEVEL block's game type (ps2x_level_type: the ladder level's
#    block, TYPE "ladder", is read as the match's type).
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
import glob

root = OUTPUT
TAG = 'patch_cutcontent'


def patch(pattern, edits, decls):
    files = glob.glob(os.path.join(root, pattern))
    assert len(files) == 1, pattern
    p = files[0]
    s = open(p, encoding='utf-8').read()
    if TAG in s:
        print('already patched', pattern)
        return
    for anchor, hook in edits:
        assert s.count(anchor) == 1, 'anchor %r in %s' % (anchor, pattern)
        s = s.replace(anchor, anchor + hook + ' // ' + TAG + '\n', 1)
    assert s.count('#include "ps2_stubs.h"\n') == 1
    s = s.replace('#include "ps2_stubs.h"\n', '#include "ps2_stubs.h"\n' + ''.join(d + '\n' for d in decls), 1)
    open(p, 'w', encoding='utf-8', newline='').write(s)
    print('patched', pattern)


PICK = 'extern "C" uint32_t ps2x_levelsel_pick(uint8_t *rdram, uint32_t shell, uint32_t sel, int move);'
patch('OnInitLevelSelect_Tag__5ShellFiPPCc_0x1d6520.cpp',
      [('    ctx->pc = 0x1d6520u;\n\n', '    ps2x_levelsel_init(rdram, GPR_U32(ctx, 4));')],
      ['extern "C" void ps2x_levelsel_init(uint8_t *rdram, uint32_t shell);'])
patch('OnLevelSelection_Tag__5ShellFiPPCc_0x1d5760.cpp',
      [('label_1d57a4:\n', '    SET_GPR_S32(ctx, 2, (int32_t)ps2x_levelsel_pick(rdram, GPR_U32(ctx, 16), GPR_U32(ctx, 2), 1));'),
       ('    SET_GPR_S32(ctx, 3, (int32_t)READ32(ADD32(GPR_U32(ctx, 2), 36)));\n',
        '    SET_GPR_S32(ctx, 3, (int32_t)ps2x_levelsel_unlock(GPR_U32(ctx, 3), GPR_U32(ctx, 17)));')],
      [PICK, 'extern "C" uint32_t ps2x_levelsel_unlock(uint32_t mask, uint32_t level);'])
patch('OnSetGameLevel_Tag__5ShellFiPPCc_0x1d6200.cpp',
      [('label_1d6218:\n', '    SET_GPR_S32(ctx, 2, (int32_t)ps2x_levelsel_pick(rdram, GPR_U32(ctx, 16), GPR_U32(ctx, 2), 0));')],
      [PICK])
patch('InBetween_set_level_texture__FPCcb_0x169040.cpp',
      [('    ctx->pc = 0x169040u;\n\n',
        '    { uint32_t flag = GPR_U32(ctx, 5); SET_GPR_S32(ctx, 4, (int32_t)ps2x_level_loadscreen(rdram, GPR_U32(ctx, 4), &flag));'
        ' SET_GPR_S32(ctx, 5, (int32_t)flag); }')],
      ['extern "C" uint32_t ps2x_level_loadscreen(uint8_t *rdram, uint32_t name, uint32_t *flag);'])
patch('LoadVehicle__13VehicleLoaderFiPCc_0x233730.cpp',
      [('    ctx->pc = 0x233730u;\n\n', '    SET_GPR_S32(ctx, 5, (int32_t)ps2x_vehicle_name(rdram, GPR_U32(ctx, 5)));')],
      ['extern "C" uint32_t ps2x_vehicle_name(uint8_t *rdram, uint32_t name);'])
patch('Type__19LevelParseSemanticsFPc_0x21b760.cpp',
      [('    ctx->pc = 0x21b760u;\n\n', '    SET_GPR_S32(ctx, 5, (int32_t)ps2x_level_type(rdram, GPR_U32(ctx, 5)));')],
      ['extern "C" uint32_t ps2x_level_type(uint8_t *rdram, uint32_t name);'])
patch('PlayMode_init_mode__Fv_0x19cf80.cpp',
      [('label_19d030:\n', '    ps2x_level_file(rdram, GPR_U32(ctx, 29) + 0x120u);')],
      ['extern "C" void ps2x_level_file(uint8_t *rdram, uint32_t buf);'])
