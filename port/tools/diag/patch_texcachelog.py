# Debug: PS2X_TEXCACHE_LOG=<start sec> logs RenderWare's PS2 texture cache bookkeeping (EE side):
#   [tc] UP raster=.. WxHxD        -> _skyTexCacheUploadRaster entry (slot about to be (re)assigned)
#   [tc] AC raster=.. entry=.. tbp=.. up=0/1 WxHxD -> RpSkyTexCacheAccessRaster after the cache decision
# A later AC with up=0 whose tbp range was given to another raster since its last UP means the game binds
# stale VRAM (garbage flares/particles) -- bookkeeping failure in the recompiled RW code. If that never
# happens, the VRAM contents themselves are wrong (lost/misplaced upload on our side).
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
import glob, os

out = OUTPUT
TAG = '// patch_texcachelog'
BS = chr(92)
HELPERS = ('#ifndef PS2TCLOG_DEFINED\n#define PS2TCLOG_DEFINED\n'
           'namespace ps2tclog {\n'
           'inline double startSec() { static const double s = std::getenv("PS2X_TEXCACHE_LOG") ? std::atof(std::getenv("PS2X_TEXCACHE_LOG")) : -1.0; return s; }\n'
           'inline bool on() { static const auto t0 = std::chrono::steady_clock::now(); const double s = startSec(); if (s < 0) return false;\n'
           '    static int left = 400000; if (left <= 0) return false; if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < s) return false; --left; return true; }\n'
           'inline bool &uploaded() { static bool u = false; return u; }\n'
           'inline uint32_t rd(const uint8_t *rdram, uint32_t a) { uint32_t v; std::memcpy(&v, rdram + (a & 0x1FFFFFFu), 4); return v; }\n'
           '}\n#endif\n')

def patch(fn, anchor, code):
    p = glob.glob(os.path.join(out, fn))[0]
    s = open(p, encoding='utf-8').read()
    if TAG in s:
        print(fn, 'already'); return
    s = s.replace('#ifdef PS2_FUNCTION_LOG_TRACKER\n#include "ps2_log.h"\n#endif\n',
                  '#ifdef PS2_FUNCTION_LOG_TRACKER\n#include "ps2_log.h"\n#endif\n#include <chrono>\n' + HELPERS, 1)
    assert s.count(anchor) == 1, (fn, anchor)
    s = s.replace(anchor, anchor + code, 1)
    open(p, 'w', encoding='utf-8', newline='').write(s)
    print('patched', fn)

# Upload entry: a0 = raster.
patch('_skyTexCacheUploadRaster_0x357db0.cpp', '    ctx->pc = 0x357db0u;\n\n',
      '    { ' + TAG + '\n'
      '        ps2tclog::uploaded() = true;\n'
      '        if (ps2tclog::on()) {\n'
      '            const uint32_t r = GPR_U32(ctx, 4);\n'
      '            std::printf("[tc] UP raster=%08x %ux%ux%u' + BS + 'n", r, ps2tclog::rd(rdram, r + 12), ps2tclog::rd(rdram, r + 16), ps2tclog::rd(rdram, r + 20));\n'
      '        }\n'
      '    }\n')

# Access: after the upload decision (label 0x358ba0: lw $v0, 0x58($s0)); s2 = raster, s0 = raster ext.
patch('RpSkyTexCacheAccessRaster_0x358b20.cpp', 'label_358ba0:\n',
      '    { ' + TAG + '\n'
      '        if (ps2tclog::on()) {\n'
      '            const uint32_t r = GPR_U32(ctx, 18), ext = GPR_U32(ctx, 16);\n'
      '            const uint32_t entry = ps2tclog::rd(rdram, ext + 0x58);\n'
      '            const uint32_t tbp = entry ? (ps2tclog::rd(rdram, entry + 4) >> 6) & 0x3FFFu : 0xFFFFu;\n'
      '            std::printf("[tc] AC raster=%08x entry=%08x tbp=%u up=%d %ux%ux%u' + BS + 'n", r, entry, tbp, ps2tclog::uploaded() ? 1 : 0,\n'
      '                        ps2tclog::rd(rdram, r + 12), ps2tclog::rd(rdram, r + 16), ps2tclog::rd(rdram, r + 20));\n'
      '        }\n'
      '        ps2tclog::uploaded() = false;\n'
      '    }\n')
