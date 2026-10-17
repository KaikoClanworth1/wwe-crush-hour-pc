import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
p = runtime('src', 'lib', 'gs', 'ps2_gs_memory.cpp')
s = open(p, encoding='utf-8').read()
start = s.index('    static constexpr P8ColumnLookupTable ColumnTable8')
end = s.index('    }};', start) + len('    }};')

base = [[0, 4, 16, 20, 32, 36, 48, 52, 8, 12, 24, 28, 40, 44, 56, 60],
        [2, 6, 18, 22, 34, 38, 50, 54, 10, 14, 26, 30, 42, 46, 58, 62],
        [33, 37, 49, 53, 1, 5, 17, 21, 41, 45, 57, 61, 9, 13, 25, 29],
        [35, 39, 51, 55, 3, 7, 19, 23, 43, 47, 59, 63, 11, 15, 27, 31]]
rows = []
for g in range(4):
    for r in base:
        rows.append([v + g * 64 for v in r])

body = '    // PSMT8 in-block byte offsets, [y][x]. Validated bit-exact against RenderWare PS2 textures that\n'
body += '    // are uploaded as PSMCT32 and sampled as PSMT8 (see tools/gs_swizzle reference).\n'
body += '    static constexpr P8ColumnLookupTable ColumnTable8\n    {{\n'
for r in rows:
    body += '        { ' + ', '.join(f'{v:3d}' for v in r) + ' },\n'
body += '    }};'
s = s[:start] + body + s[end:]
open(p, 'w', encoding='utf-8').write(s)
print('ok')
