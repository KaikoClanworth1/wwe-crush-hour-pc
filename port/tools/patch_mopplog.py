# Debug: PS2X_MOPP_LOG=1 logs hkMoppLongRayVirtualMachine::queryLongRay calls whose segment crosses the HARDCORE
# start-room floor (y=-5.8, x -110..190, z -335..-145) and how many addRealHit callbacks (with keys) each produced.
import os, glob
BS = chr(92)
out = r'D:\Xbox Games Ports\WWE Crush Hour\recomp\output'
TAG = '// patch_mopplog'
q = glob.glob(os.path.join(out, 'queryLongRay__Q25Havok27hkMoppLongRayVirtualMachine*_0x3ae720.cpp'))[0]
h = glob.glob(os.path.join(out, 'addRealHit__Q25Havok27hkMoppLongRayVirtualMachine*_0x3ae300.cpp'))[0]
common = '''namespace ps2mopplog { inline uint32_t &hits() { static uint32_t v = 0; return v; } inline uint32_t *keys() { static uint32_t k[8]; return k; } }
'''
for p, entry_code in [(q, '''    { %s
        static const bool s_on = std::getenv("PS2X_MOPP_LOG") != nullptr;
        static bool s_have = false; static float s_q[6]; static uint32_t s_geom = 0; static int s_lines = 0;
        if (s_on && ctx->pc == 0x3ae720u)
        {
            if (s_have && s_lines < 4000)
            {
                ++s_lines;
                std::printf("[mopp] geom=%%08x from=(%%.2f %%.2f %%.2f) to=(%%.2f %%.2f %%.2f) hits=%%u", s_geom, s_q[0], s_q[1], s_q[2], s_q[3], s_q[4], s_q[5], ps2mopplog::hits());
                for (uint32_t i = 0; i < ps2mopplog::hits() && i < 8u; ++i) std::printf(" %%u", ps2mopplog::keys()[i]);
                std::printf("%sn");
            }
            s_have = false;
            ps2mopplog::hits() = 0;
            auto f = [&](uint32_t a) { float v; std::memcpy(&v, rdram + (a & 0x1FFFFFFu), 4); return v; };
            const uint32_t fa = GPR_U32(ctx, 7), ta = GPR_U32(ctx, 8);
            const float q[6] = {f(fa), f(fa + 4), f(fa + 8), f(ta), f(ta + 4), f(ta + 8)};
            const bool crosses = (q[1] > -5.8f) != (q[4] > -5.8f);
            const bool inRoom = q[0] > -110.f && q[0] < 190.f && q[2] > -335.f && q[2] < -145.f;
            if (crosses && inRoom) { s_have = true; std::memcpy(s_q, q, sizeof(q)); s_geom = GPR_U32(ctx, 5); }
        }
    }
''' % (TAG, BS)), (h, '''    { %s
        if (ctx->pc == 0x3ae300u) { uint32_t &n = ps2mopplog::hits(); if (n < 8u) ps2mopplog::keys()[n] = GPR_U32(ctx, 5); ++n; }
    }
''' % TAG)]:
    s = open(p, encoding='utf-8').read()
    if TAG in s:
        print(os.path.basename(p), 'already'); continue
    s = s.replace('#ifdef PS2_FUNCTION_LOG_TRACKER\n#include "ps2_log.h"\n#endif\n', '#ifdef PS2_FUNCTION_LOG_TRACKER\n#include "ps2_log.h"\n#endif\n' + common, 1)
    i = s.index('    switch (ctx->pc) {')
    s = s[:i] + entry_code + s[i:]
    open(p, 'w', encoding='utf-8', newline='').write(s)
    print('patched', os.path.basename(p))
