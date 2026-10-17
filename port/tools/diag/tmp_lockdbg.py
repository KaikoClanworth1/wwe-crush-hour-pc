# TEMP instrumentation: RpSkyTexCacheRasterLock args/result. Remove with --remove.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
import sys, re
path = out('RpSkyTexCacheRasterLock_0x3592f0.cpp')
s = open(path, encoding='utf-8').read()
s = re.sub(r'    \{ // TEMP-LOCKDBG.*?\n    \}\n', '', s, flags=re.S)
s = s.replace('static uint32_t g_lockDbgArgs[3];\n', '')
if '--remove' not in sys.argv:
    s = s.replace('// Function: RpSkyTexCacheRasterLock', 'static uint32_t g_lockDbgArgs[3];\n// Function: RpSkyTexCacheRasterLock', 1)
    entry = ('    { // TEMP-LOCKDBG\n'
             '        g_lockDbgArgs[0] = GPR_U32(ctx, 4); g_lockDbgArgs[1] = GPR_U32(ctx, 5); g_lockDbgArgs[2] = GPR_U32(ctx, 31);\n'
             '    }\n')
    exitb = ('    { // TEMP-LOCKDBG\n'
             '        const uint32_t r = g_lockDbgArgs[0];\n'
             '        std::printf("[lockdbg] raster=%08x lock=%u ra=%08x -> v0=%u  w=%d h=%d depth=%d type=%02x\\n", r, g_lockDbgArgs[1], g_lockDbgArgs[2], GPR_U32(ctx, 2),\n'
             '            (int)READ32(r + 0x0c), (int)READ32(r + 0x10), (int)READ32(r + 0x14), READ8(r + 0x20)); std::fflush(stdout);\n'
             '    }\n')
    a = '    ctx->pc = 0x3592f0u;\n\n'
    b = '    // 0x359744: 0x3e00008  jr          $ra\n'
    assert s.count(b) == 1
    i = s.index(a)
    s = s[:i + len(a)] + entry + s[i + len(a):]
    s = s.replace(b, exitb + b, 1)
open(path, 'w', encoding='utf-8', newline='').write(s)
print('done')
