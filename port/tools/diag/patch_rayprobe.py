# Debug: PS2X_RAY_PROBE=1 -- every 900th PhysicsManager::CastRayBatch call, cast a 41x21 grid of vertical rays
# (2-unit spacing) around the first ray's origin through the game's own raycaster and print a hit map
# ('#' hit, '.' miss, 'C' car). Shows holes in an arena's collision world. Saves/restores the CPU context and
# the game's ray/result arrays. Needs tools/patch_raylog.py (entry capture) applied first.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
import glob, os
BS = chr(92)
out = OUTPUT
p = glob.glob(os.path.join(out, 'CastRayBatch__14PhysicsManager*_0x195110.cpp'))[0]
import re; fn = re.search(r'^void (\w+)\(uint8_t\* rdram', open(p, encoding='utf-8').read(), re.M).group(1)
s = open(p, encoding='utf-8').read()
TAG = '// patch_rayprobe'
if TAG not in s:
    a = '    s_rayN = GPR_U32(ctx, 4); s_rayFrom = GPR_U32(ctx, 5); s_rayTo = GPR_U32(ctx, 6); s_rayRes = GPR_U32(ctx, 8);\n'
    assert s.count(a) == 1
    s = s.replace(a, a + '    static thread_local uint32_t s_rayMask, s_rayIgnore; s_rayMask = GPR_U32(ctx, 7); s_rayIgnore = GPR_U32(ctx, 9); ' + TAG + '\n', 1)
    ret = 'label_195144:\n'
    block = '''    { ''' + TAG + '''
        static const bool s_probe = std::getenv("PS2X_RAY_PROBE") != nullptr;
        static bool s_inProbe = false;
        static uint32_t s_probeCalls = 0;
        float s_ny = 0.0f; std::memcpy(&s_ny, rdram + ((s_rayRes + 36u) & 0x1FFFFFFu), 4);
        if (s_probe && !s_inProbe && s_rayN >= 1u && (rdram[s_rayRes & 0x1FFFFFFu] & 0xFFu) && s_ny > 0.9f && (++s_probeCalls % 300u) == 0u)
        {
            s_inProbe = true;
            const R5900Context saved = *ctx;
            const uint32_t from = s_rayFrom & 0x1FFFFFFu, to = s_rayTo & 0x1FFFFFFu, res = s_rayRes & 0x1FFFFFFu;
            uint8_t keepFrom[16], keepTo[16], keepRes[96];
            std::memcpy(keepFrom, rdram + from, 16);
            std::memcpy(keepTo, rdram + to, 16);
            std::memcpy(keepRes, rdram + res, 96);
            float c[3];
            std::memcpy(c, rdram + from, 12);
            const uint32_t mask = s_rayMask, ignore = s_rayIgnore;
            auto probeHit = [&](uint32_t r) { uint32_t body; std::memcpy(&body, rdram + r + 20, 4); return body != 0u && body != 0xFFFFFFFFu; };
            std::printf("[probe] around (%.1f %.1f %.1f), x left->right, z top->bottom, 2 units/cell'''+BS+'''n", c[0], c[1], c[2]);
            for (int gz = -10; gz <= 10; ++gz)
            {
                char line[64];
                int k = 0;
                for (int gx = -20; gx <= 20; ++gx)
                {
                    const float x = c[0] + gx * 2.0f, z = c[2] + gz * 2.0f;
                    float f0[4] = {x, c[1] + 4.0f, z, 1.0f}, t0[4] = {x, c[1] - 12.0f, z, 1.0f};
                    if (gx == 0 && gz == 0) { std::memcpy(f0, keepFrom, 16); std::memcpy(t0, keepTo, 16); } // exact game ray
                    std::memcpy(rdram + from, f0, 16);
                    std::memcpy(rdram + to, t0, 16);
                    std::memcpy(rdram + res, keepRes, 96); // fresh query: no hit, farthest distance
                    { const uint32_t none = 0u; std::memcpy(rdram + res + 20, &none, 4); const float far = 3.0e38f; std::memcpy(rdram + res + 28, &far, 4); }
                    SET_GPR_U32(ctx, 4, 1u); SET_GPR_U32(ctx, 5, s_rayFrom); SET_GPR_U32(ctx, 6, s_rayTo);
                    SET_GPR_U32(ctx, 7, mask); SET_GPR_U32(ctx, 8, s_rayRes); SET_GPR_U32(ctx, 9, ignore);
                    SET_GPR_U32(ctx, 29, saved.r[29].m128i_u32[0] - 0x400u);
                    SET_GPR_U32(ctx, 31, 0x195144u);
                    ''' + fn + '''(rdram, ctx, runtime);
                    line[k++] = (gx == 0 && gz == 0) ? (probeHit(res) ? 'H' : 'm') : (probeHit(res) ? '#' : '.');
                }
                line[k] = 0;
                std::printf("[probe] %s'''+BS+'''n", line);
            }
            std::fflush(stdout);
            std::memcpy(rdram + from, keepFrom, 16);
            std::memcpy(rdram + to, keepTo, 16);
            std::memcpy(rdram + res, keepRes, 96);
            *ctx = saved;
            s_inProbe = false;
        }
    }
'''
    assert s.count(ret) == 1
    s = s.replace(ret, ret + block, 1)
    open(p, 'w', encoding='utf-8', newline='').write(s)
print('ok')
