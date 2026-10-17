# EE FPU semantics in generated code (recompiler fixed too; macros live in the PCH header):
#  - CVT.W.S truncates toward zero and saturates (the EE FPU only rounds toward zero)
#  - SQRT.S of a negative value returns sqrt(|x|)
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
import glob, re
cvt = re.compile(r'FPU_CVT_W_S\(ctx->f\[(\d+)\]\)')
sq = re.compile(r'FPU_SQRT_S\(ctx->f\[(\d+)\]\)')
def cvt_rep(m):
    r = f'ctx->f[{m.group(1)}]'
    return (f'((fabsf({r}) >= 2147483648.0f) ? (({r}) < 0.0f ? INT32_MIN : INT32_MAX) : (int32_t)({r}))')
files = 0
for f in glob.glob(out('*.cpp')):
    s = open(f, encoding='utf-8', errors='surrogateescape').read()
    if 'FPU_CVT_W_S(' not in s and 'FPU_SQRT_S(' not in s:
        continue
    n = cvt.sub(cvt_rep, s)
    n = sq.sub(lambda m: f'sqrtf(fabsf(ctx->f[{m.group(1)}]))', n)
    if n != s:
        open(f, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(n)
        files += 1
print('patched files', files)
