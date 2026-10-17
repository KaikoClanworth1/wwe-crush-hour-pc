# Debug: log PhysicsBodyCore::PushMovePosition(this, const Vector &target) (keyframed bodies such as the
# respawn elevators). PS2X_PUSHMOVE_LOG=1 prints every 15th call per body for the first 8 bodies:
# target, the previous target (+0x30) and the stored velocity (+0x40) from the last call.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
import os

p = out('PushMovePosition__15PhysicsBodyCoreFRCQ211PhysicsMath6Vector_0x1878f0.cpp')
TAG = '// patch_pushmove_log'
s = open(p, encoding='utf-8').read()
if TAG not in s:
    anchor = '    ctx->pc = 0x1878f0u;\n\n'
    assert s.count(anchor) == 1
    block = '''    { ''' + TAG + '''
        static const bool s_on = std::getenv("PS2X_PUSHMOVE_LOG") != nullptr;
        if (s_on)
        {
            static uint32_t s_bodies[8] = {}, s_calls[8] = {};
            const uint32_t self = GPR_U32(ctx, 4), tgt = GPR_U32(ctx, 5);
            int slot = -1;
            for (int i = 0; i < 8; ++i)
                if (s_bodies[i] == self || s_bodies[i] == 0u) { s_bodies[i] = self; slot = i; break; }
            if (slot >= 0 && (s_calls[slot]++ % 15u) == 0u)
            {
                auto f = [&](uint32_t a) { float v; std::memcpy(&v, rdram + (a & 0x1FFFFFFu), 4); return v; };
                uint32_t slice;
                std::memcpy(&slice, rdram + 0x497588u, 4);
                std::printf("[pushmove] body=%08x n=%u tgt=(%.2f %.2f %.2f) prev=(%.2f %.2f %.2f) +20=(%.2f %.2f %.2f) vel=(%.2f %.2f %.2f) slice=%u\\n",
                            self, s_calls[slot], f(tgt), f(tgt + 4), f(tgt + 8), f(self + 0x30), f(self + 0x34), f(self + 0x38),
                            f(self + 0x20), f(self + 0x24), f(self + 0x28), f(self + 0x40), f(self + 0x44), f(self + 0x48), slice);
                std::fflush(stdout);
            }
        }
    }
'''
    s = s.replace(anchor, anchor + block, 1)
    open(p, 'w', encoding='utf-8', newline='').write(s)
print('ok')
