# EE float division in generated code (recompiler fixed too: fpu_translator.cpp, vu_translation_helpers.cpp):
#  - DIV.S:  x / 0 was copysignf(INFINITY, ...); the EE gives +-FLT_MAX (sign fs ^ ft) -> ps2_fdiv
#  - VDIV Q: x / 0 was 0                                                           -> ps2_fdiv
# ps2_fdiv / ps2_fclamp live in ps2_runtime_macros.h (FPU_ADD/SUB/MUL_S and PS2_V* clamp overflow there).
import glob, re

div = re.compile(r'if \(ctx->f\[(\d+)\] == 0\.0f\) \{ ctx->fcr31 \|= 0x100000; /\* DZ flag \*/ '
                 r'ctx->f\[(\d+)\] = copysignf\(INFINITY, ctx->f\[(\d+)\] \* 0\.0f\); \} '
                 r'else ctx->f\[\2\] = ctx->f\[\3\] / ctx->f\[\1\];')
vdiv = 'ctx->vu0_q = (ft != 0.0f) ? (fs / ft) : 0.0f;'


def div_rep(m):
    ft, fd, fs = m.group(1), m.group(2), m.group(3)
    return (f'if (ctx->f[{ft}] == 0.0f) ctx->fcr31 |= 0x100000; /* DZ flag */ '
            f'ctx->f[{fd}] = ps2_fdiv(ctx->f[{fs}], ctx->f[{ft}]);')


files = sites = 0
for f in glob.glob(r'D:\Xbox Games Ports\WWE Crush Hour\recomp\output\*.cpp'):
    s = open(f, encoding='utf-8', errors='surrogateescape').read()
    if 'INFINITY' not in s and vdiv not in s:
        continue
    n, k = div.subn(div_rep, s)
    sites += k + n.count(vdiv)
    n = n.replace(vdiv, 'ctx->vu0_q = ps2_fdiv(fs, ft);')
    if n != s:
        open(f, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(n)
        files += 1
print('patched files', files, 'sites', sites)
