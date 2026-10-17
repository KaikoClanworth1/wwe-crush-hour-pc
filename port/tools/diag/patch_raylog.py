# Debug: PS2X_RAY_LOG=1 logs PhysicsManager::CastRayBatch(n, from[], to[], mask, results[], ignore) every
# 30th call: ray 0 endpoints and the first 16 result words (layout discovery for floor-hole hunting).
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
import os, glob
BS = chr(92)
out = OUTPUT
p = glob.glob(os.path.join(out, 'CastRayBatch__14PhysicsManager*_0x195110.cpp'))[0]
s = open(p, encoding='utf-8').read()
TAG = '// patch_raylog'
if TAG not in s:
    entry = '    ctx->pc = 0x195110u;\n\n'
    assert s.count(entry) == 1
    s = s.replace(entry, entry + '''    static thread_local uint32_t s_rayN, s_rayFrom, s_rayTo, s_rayRes; ''' + TAG + '''
    s_rayN = GPR_U32(ctx, 4); s_rayFrom = GPR_U32(ctx, 5); s_rayTo = GPR_U32(ctx, 6); s_rayRes = GPR_U32(ctx, 8);
''', 1)
    ret = 'label_195144:\n'
    assert s.count(ret) == 1
    s = s.replace(ret, ret + '''    { ''' + TAG + '''
        static const bool s_on = std::getenv("PS2X_RAY_LOG") != nullptr;
        static uint32_t s_calls = 0;
        if (s_on && (s_calls++ % 30u) == 0u)
        {
            auto f = [&](uint32_t a) { float v; std::memcpy(&v, rdram + (a & 0x1FFFFFFu), 4); return v; };
            auto w = [&](uint32_t a) { uint32_t v; std::memcpy(&v, rdram + (a & 0x1FFFFFFu), 4); return v; };
            std::printf("[ray] n=%u from=(%.2f %.2f %.2f) to=(%.2f %.2f %.2f) res:", s_rayN, f(s_rayFrom), f(s_rayFrom + 4),
                        f(s_rayFrom + 8), f(s_rayTo), f(s_rayTo + 4), f(s_rayTo + 8));
            for (uint32_t i = 0; i < 16u; ++i)
                std::printf(" %08x", w(s_rayRes + i * 4u));
            std::printf("''' + BS + '''n");
            std::fflush(stdout);
        }
    }
''', 1)
    open(p, 'w', encoding='utf-8', newline='').write(s)
print('ok')
