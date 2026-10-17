# 60 fps: the game's frame limiter (ps2_wait_for_vsync @ 0x1771a0) spins until the VSync counter at
# 0x49A9F0 reaches 2 (sltiu $at, $v1, 2 @ 0x1771BC), i.e. 30 fps. Make the threshold come from the
# host (ps2x_frame_vsync_threshold(): 1 when TargetFPS >= 60). GameClock is a real-time stopwatch and
# Havok steps with a variable dt, so game speed stays correct.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
p = out('ps2_wait_for_vsync__Fv_0x1771a0.cpp')
s = open(p, encoding='utf-8').read()
old = 'SET_GPR_U64(ctx, 1, ((uint64_t)GPR_U64(ctx, 3) < (uint64_t)(int64_t)(int32_t)2) ? 1 : 0);'
new = 'SET_GPR_U64(ctx, 1, ((uint64_t)GPR_U64(ctx, 3) < (uint64_t)ps2x_frame_vsync_threshold()) ? 1 : 0);'
if new not in s:
    assert old in s
    s = s.replace(old, new, 1)
    s = s.replace('#include "ps2_stubs.h"', '#include "ps2_stubs.h"\nextern "C" uint32_t ps2x_frame_vsync_threshold();', 1)
    open(p, 'w', encoding='utf-8', newline='').write(s)
print('ok')
