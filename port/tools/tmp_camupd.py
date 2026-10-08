# TEMP instrumentation: SkyCameraBeginUpdate(out, camera, flags) camera raster info. Remove with --remove.
import sys, re
path = r'D:\Xbox Games Ports\WWE Crush Hour\recomp\output\SkyCameraBeginUpdate_0x3414b0.cpp'
s = open(path, encoding='utf-8').read()
s = re.sub(r'    \{ // TEMP-CAMUPD.*?\n    \}\n', '', s, flags=re.S)
s = s.replace('static const auto g_camUpdT0 = std::chrono::steady_clock::now();\n', '')
if '--remove' not in sys.argv:
    if '#include <chrono>' not in s:
        s = s.replace('#include <stdexcept>', '#include <stdexcept>\n#include <chrono>', 1)
    s = s.replace('// Function: SkyCameraBeginUpdate', 'static const auto g_camUpdT0 = std::chrono::steady_clock::now();\n// Function: SkyCameraBeginUpdate', 1)
    blk = ('    { // TEMP-CAMUPD\n'
           '        static int logged = 0;\n'
           '        if (std::chrono::steady_clock::now() - g_camUpdT0 > std::chrono::seconds(150) && logged < 60) {\n'
           '            ++logged;\n'
           '            const uint32_t cam = GPR_U32(ctx, 5), r = READ32(cam + 0x60), z = READ32(cam + 0x64);\n'
           '            std::printf("[camupd] cam=%08x ra=%08x raster=%08x %dx%d d%d type=%02x parent=%08x off=%d,%d zr=%08x\\n", cam, GPR_U32(ctx, 31), r,\n'
           '                (int)READ32(r + 0x0c), (int)READ32(r + 0x10), (int)READ32(r + 0x14), READ8(r + 0x20), READ32(r), (int)(int16_t)READ16(r + 0x1c), (int)(int16_t)READ16(r + 0x1e), z); std::fflush(stdout); }\n'
           '    }\n')
    a = '    ctx->pc = 0x3414b0u;\n\n'
    assert s.count(a) >= 1
    i = s.index(a)
    s = s[:i + len(a)] + blk + s[i + len(a):]
open(path, 'w', encoding='utf-8', newline='').write(s)
print('done')
