# Debug: PS2X_VIDTEX_LOG=1 logs VidTexManager::Request(char*, char*, ...) and VidTex::Open(char*, ...) path arguments.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
import glob, os

out = OUTPUT
TAG = '// patch_vidtexlog'
BS = chr(92)
for pat, label in [('Request__13VidTexManagerFPcPcUiUiP9RwTexturef_0x234e20.cpp', 'Request'),
                   ('Open__6VidTexFPcUiUiP15PcplAllocRegion_0x29b710.cpp', 'Open')]:
    p = os.path.join(out, pat)
    s = open(p, encoding='utf-8').read()
    if TAG in s:
        print(pat, 'already'); continue
    entry = int(pat.rsplit('_0x', 1)[1][:-4], 16)
    blk = ('    { ' + TAG + '\n'
           '        static const bool s_on = std::getenv("PS2X_VIDTEX_LOG") != nullptr;\n'
           '        if (s_on && ctx->pc == 0x%xu) {\n' % entry +
           '            auto str = [&](int r) -> const char * { const uint32_t a = GPR_U32(ctx, r) & 0x1FFFFFFu; return a ? reinterpret_cast<const char *>(rdram + a) : "(null)"; };\n'
           '            std::printf("[vidtex] ' + label + ' a1=%.80s a2=%.80s a3=%08x t0=%08x' + BS + 'n", str(5), str(6), GPR_U32(ctx, 7), GPR_U32(ctx, 8));\n'
           '            std::fflush(stdout);\n'
           '        }\n'
           '    }\n')
    i = s.index('    switch (ctx->pc) {')
    s = s[:i] + blk + s[i:]
    open(p, 'w', encoding='utf-8', newline='').write(s)
    print('patched', pat)
