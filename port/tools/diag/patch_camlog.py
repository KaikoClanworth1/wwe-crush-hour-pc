# Generated code: log every RwCameraBeginUpdate in a window anchored to PS2X_TRACE_ANCHOR_PC's first call
# (PS2X_CAMLOG_ANCHOR=<sec>, PS2X_CAMLOG_LEN=<sec>, default 1 s, cap 3000 lines): camera, its frame's LTM
# position / at-vector, raster (size, offsets, type) and view window. Used to see whether the object pass
# (RenderImageList::render re-begins a camera) projects with a different camera/pose than the world pass.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
import re

path = out('RwCameraBeginUpdate_0x32ff30.cpp')
s = open(path, encoding='utf-8').read()
s = re.sub(r'    \{ // patch_camlog.*?\n    \}\n', '', s, flags=re.S)  # idempotent

hook = r'''    { // patch_camlog
        static const double s_from = std::getenv("PS2X_CAMLOG_ANCHOR") ? std::atof(std::getenv("PS2X_CAMLOG_ANCHOR")) : -1.0;
        static const double s_len = std::getenv("PS2X_CAMLOG_LEN") ? std::atof(std::getenv("PS2X_CAMLOG_LEN")) : 1.0;
        static int s_left = 3000;
        extern std::atomic<double> g_ps2xTraceAnchorAt;
        const double a = g_ps2xTraceAnchorAt.load();
        if (s_from >= 0.0 && a >= 0.0 && s_left > 0 && ctx->pc == 0x32ff30u)
        {
            const double rel = ps2host::timeSeconds() - a;
            if (rel >= s_from && rel < s_from + s_len)
            {
                --s_left;
                const uint32_t cam = GPR_U32(ctx, 4);
                auto rd = [&](uint32_t p) { uint32_t v; std::memcpy(&v, rdram + (p & 0x1FFFFFFu), 4); return v; };
                auto rf = [&](uint32_t p) { float v; std::memcpy(&v, rdram + (p & 0x1FFFFFFu), 4); return v; };
                const uint32_t frame = rd(cam + 4), ras = rd(cam + 0x60);
                std::printf("[camlog] t=%.2f cam=%08x frame=%08x pos=(%.1f,%.1f,%.1f) at=(%.2f,%.2f,%.2f) ras=%08x %ux%u off=(%d,%d) type=%u vw=(%.3f,%.3f) near=%.2f far=%.1f\n",
                            rel, cam, frame, rf(frame + 0x80), rf(frame + 0x84), rf(frame + 0x88), rf(frame + 0x70), rf(frame + 0x74), rf(frame + 0x78),
                            ras, rd(ras + 0xC), rd(ras + 0x10), (int)(int16_t)(rd(ras + 0x1C) & 0xFFFF), (int)(int16_t)(rd(ras + 0x1C) >> 16),
                            rd(ras + 0x20) & 0xFF, rf(cam + 0x68), rf(cam + 0x6C), rf(cam + 0xA0), rf(cam + 0xA4));
            }
        }
    }
'''
marker = 'void RwCameraBeginUpdate_0x32ff30(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {\n'
assert marker in s
# insert after the PS_LOG_ENTRY block (first "#endif" after the marker)
i = s.index(marker) + len(marker)
j = s.index('#endif\n', i) + len('#endif\n')
s = s[:j] + hook + s[j:]
for inc in ('#include <atomic>', '#include <cstdlib>', '#include <cstring>', '#include <cstdio>'):
    if inc not in s:
        s = s.replace('#include <stdexcept>', '#include <stdexcept>\n' + inc, 1)
if 'namespace ps2host { double timeSeconds(); }' not in s:
    s = s.replace('#include <stdexcept>', '#include <stdexcept>\nnamespace ps2host { double timeSeconds(); }', 1)
open(path, 'w', encoding='utf-8', newline='').write(s)
print('patched')
