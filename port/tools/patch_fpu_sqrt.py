# Patch generated code: EE SQRT.S takes its operand from the ft field (recompiler used fs).
import glob, re
pat = re.compile(r'(// 0x([0-9a-f]+): 0x([0-9a-f]+)[^\n]*\n(?:[^\n]*\n){0,2}?\s*ctx->f\[(\d+)\] = FPU_SQRT_S\(ctx->f\[)(\d+)(\]\);)')
files = changed = 0
for f in glob.glob('recomp/output/*.cpp'):
    s = open(f, encoding='utf-8', errors='surrogateescape').read()
    if 'FPU_SQRT_S(ctx->f[' not in s:
        continue
    def fix(m):
        global changed
        word = int(m.group(3), 16)
        assert (word >> 26) == 0x11 and (word & 0x3F) == 4, hex(word)
        ft = (word >> 16) & 31
        changed += 1
        return m.group(1) + str(ft) + m.group(6)
    n = pat.sub(fix, s)
    if n != s:
        open(f, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(n)
        files += 1
print('files', files, 'sites', changed)
