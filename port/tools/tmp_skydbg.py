# TEMP instrumentation: SkyStart VRAM accounting before skyTexCacheOpenCache. Remove with --remove.
import sys, re
path = r'D:\Xbox Games Ports\WWE Crush Hour\recomp\output\SkyStart_0x34c1c0.cpp'
s = open(path, encoding='utf-8').read()
s = re.sub(r'    \{ // TEMP-SKYDBG.*?\n    \}\n', '', s, flags=re.S)
if '--remove' not in sys.argv:
    # Log every store to the VRAM-used counter (gp-0x7998) and the final cache range.
    n = 0
    def repl(m):
        global n
        n += 1
        return m.group(0) + ('    { // TEMP-SKYDBG\n'
                             '        std::printf("[skydbg] pc=%08x vramUsed=%08x s1=%08x s3=%08x a2=%08x\\n", ctx->pc, READ32(ADD32(GPR_U32(ctx, 28), 0xFFFF8668u)), GPR_U32(ctx, 17), GPR_U32(ctx, 19), GPR_U32(ctx, 6)); std::fflush(stdout);\n'
                             '    }\n')
    s = re.sub(r'    // 0x[0-9a-f]+: 0x[0-9a-f]+  sw          \$\w+, -0x7998\(\$gp\)\n    ctx->pc = 0x[0-9a-f]+u;\n    [^\n]*\n', repl, s)
    print('sites', n)
open(path, 'w', encoding='utf-8', newline='').write(s)
print('done')
