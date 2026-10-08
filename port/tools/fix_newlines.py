# Repairs C string literals inside printf(...) calls that were split by a literal newline.
import re, sys

for path in sys.argv[1:]:
    s = open(path, encoding='utf-8').read()
    out = []
    i = 0
    n = len(s)
    fixed = 0
    while i < n:
        m = s.find('printf("', i)
        if m < 0:
            out.append(s[i:])
            break
        out.append(s[i:m + 8])
        j = m + 8
        buf = []
        while j < n and s[j] != '"':
            if s[j] == '\\' and j + 1 < n:
                buf.append(s[j:j + 2])
                j += 2
                continue
            if s[j] == '\n':
                buf.append('\\n')
                fixed += 1
            else:
                buf.append(s[j])
            j += 1
        out.append(''.join(buf))
        i = j
    s2 = ''.join(out)
    open(path, 'w', encoding='utf-8').write(s2)
    print(path, 'fixed', fixed)
