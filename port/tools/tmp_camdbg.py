# TEMP instrumentation: ValidatePhysicalCamera inputs/outputs. Remove with --remove.
import sys, re, glob
path = glob.glob(r'D:\Xbox Games Ports\WWE Crush Hour\recomp\output\ValidatePhysicalCamera*_0x1e45f0.cpp')[0]
s = open(path, encoding='utf-8').read()
s = re.sub(r'    \{ // TEMP-CAMDBG.*?\n    \}\n', '', s, flags=re.S)
s = s.replace('uint32_t g_camDbgPtr[4]; bool g_camDbgOn;\n', '')
if '--remove' not in sys.argv:
    VEC = ('auto vec = [&](uint32_t p) { float v[3]; for (int k = 0; k < 3; ++k) { uint32_t b = READ32(p + 4u * k); '
           'std::memcpy(&v[k], &b, 4); } std::printf(" (%.2f %.2f %.2f)", v[0], v[1], v[2]); };')
    entry = ('    { // TEMP-CAMDBG\n'
             '        static uint64_t n = 0; ++n; extern uint32_t g_camDbgPtr[4]; extern bool g_camDbgOn;\n'
             '        g_camDbgOn = (n % 60u) == 1u;\n'
             '        g_camDbgPtr[0] = GPR_U32(ctx, 4); g_camDbgPtr[1] = GPR_U32(ctx, 5); g_camDbgPtr[2] = GPR_U32(ctx, 6); g_camDbgPtr[3] = GPR_U32(ctx, 8);\n'
             '        if (g_camDbgOn) { ' + VEC + ' std::printf("[camdbg] in n=%llu f12=%.3f f13=%.3f f14=%.3f f15=%.3f a3=%u ra=%08x", (unsigned long long)n, ctx->f[12], ctx->f[13], ctx->f[14], ctx->f[15], GPR_U32(ctx, 7), GPR_U32(ctx, 31));\n'
             '            vec(GPR_U32(ctx, 4)); vec(GPR_U32(ctx, 5)); vec(GPR_U32(ctx, 6)); std::printf("\\n"); }\n'
             '    }\n')
    exitb = ('    { // TEMP-CAMDBG\n'
             '        extern uint32_t g_camDbgPtr[4]; extern bool g_camDbgOn;\n'
             '        if (g_camDbgOn) { ' + VEC + ' std::printf("[camdbg] out v0=%u f0=%.3f", GPR_U32(ctx, 2), ctx->f[0]);\n'
             '            vec(g_camDbgPtr[0]); vec(g_camDbgPtr[1]); vec(g_camDbgPtr[2]); std::printf(" t0v=%.3f\\n", [&]{ uint32_t b = READ32(g_camDbgPtr[3]); float f; std::memcpy(&f, &b, 4); return f; }()); std::fflush(stdout); }\n'
             '    }\n')
    a = '    ctx->pc = 0x1e45f0u;\n\n'
    b = '    // 0x1e4f30: 0x3e00008  jr          $ra\n'
    assert s.count(a) == 1 and s.count(b) == 1
    s = s.replace(a, a + entry, 1).replace(b, exitb + b, 1)
    s = s.replace('// Function: ValidatePhysicalCamera', 'uint32_t g_camDbgPtr[4]; bool g_camDbgOn;\n// Function: ValidatePhysicalCamera', 1)
open(path, 'w', encoding='utf-8', newline='').write(s)
print('done')
