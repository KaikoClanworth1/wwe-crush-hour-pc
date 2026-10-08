# Compare GS-memory-model deswizzle (CT32 upload -> T8 read) using two PSMT8 column tables against the
# classic, widely used RenderWare/PS2 8-bit unswizzle formula.
import random

BLOCK32 = [[0,1,4,5,16,17,20,21],[2,3,6,7,18,19,22,23],[8,9,12,13,24,25,28,29],[10,11,14,15,26,27,30,31]]
COL32 = [[0,1,4,5,8,9,12,13],[2,3,6,7,10,11,14,15],[16,17,20,21,24,25,28,29],[18,19,22,23,26,27,30,31],
         [32,33,36,37,40,41,44,45],[34,35,38,39,42,43,46,47],[48,49,52,53,56,57,60,61],[50,51,54,55,58,59,62,63]]

def expand(base, groups, step):
    out = []
    for g in range(groups):
        for row in base:
            out.append([v + g*step for v in row])
    return out

PCSX2_C8 = expand([[0,4,16,20,32,36,48,52,2,6,18,22,34,38,50,54],
                   [8,12,24,28,40,44,56,60,10,14,26,30,42,46,58,62],
                   [33,37,49,53,1,5,17,21,35,39,51,55,3,7,19,23],
                   [41,45,57,61,9,13,25,29,43,47,59,63,11,15,27,31]], 4, 64)
FPROJ_C8 = expand([[0,4,16,20,32,36,48,52,8,12,24,28,40,44,56,60],
                   [2,6,18,22,34,38,50,54,10,14,26,30,42,46,58,62],
                   [33,37,49,53,1,5,17,21,41,45,57,61,9,13,25,29],
                   [35,39,51,55,3,7,19,23,43,47,59,63,11,15,27,31]], 4, 64)

def addr32(x, y, bw):  # byte address, bw in pages (64px)
    page = (y // 32) * bw + (x // 64)
    blk = BLOCK32[(y % 32) // 8][(x % 64) // 8]
    return page * 8192 + blk * 256 + COL32[y % 8][x % 8] * 4

def addr8(x, y, bw_pages, col):  # bw_pages = texture width / 128
    page = (y // 64) * bw_pages + (x // 128)
    blk = BLOCK32[(y % 64) // 16][(x % 128) // 16]
    return page * 8192 + blk * 256 + col[y % 16][x % 16]

def gs_model(host, w, h, col):
    uw, uh = w // 2, h // 2
    mem = bytearray(1 << 20)
    i = 0
    for y in range(uh):
        for x in range(uw):
            a = addr32(x, y, max(1, uw // 64))
            mem[a:a+4] = host[i:i+4]
            i += 4
    return bytes(mem[addr8(x, y, max(1, w // 128), col)] for y in range(h) for x in range(w))

def classic(host, w, h):
    out = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            block_location = (y & ~0xF) * w + (x & ~0xF) * 2
            swap_selector = (((y + 2) >> 2) & 0x1) * 4
            posY = (((y & ~3) >> 1) + (y & 1)) & 0x7
            column_location = posY * w * 2 + ((x + swap_selector) & 0x7) * 4
            byte_num = ((y >> 1) & 1) + ((x >> 2) & 2)
            out[y * w + x] = host[block_location + column_location + byte_num]
    return bytes(out)

for (w, h) in [(128, 128), (256, 256), (256, 128)]:
    host = bytes(random.randrange(256) for _ in range(w * h))
    ref = classic(host, w, h)
    for name, col in [('pcsx2', PCSX2_C8), ('fproj', FPROJ_C8)]:
        got = gs_model(host, w, h, col)
        same = sum(1 for a, b in zip(ref, got) if a == b)
        print(f'{w}x{h} {name}: {same}/{w*h} match')

import itertools
base = [[0,4,16,20,32,36,48,52,2,6,18,22,34,38,50,54],
        [8,12,24,28,40,44,56,60,10,14,26,30,42,46,58,62],
        [33,37,49,53,1,5,17,21,35,39,51,55,3,7,19,23],
        [41,45,57,61,9,13,25,29,43,47,59,63,11,15,27,31]]
w = h = 128
host = bytes(random.randrange(256) for _ in range(w * h))
ref = classic(host, w, h)
best = []
vals = sorted(set(v for r in base for v in r))
for perm in itertools.permutations(range(4)):
    for flip in range(16):
        rows = []
        for i, pi in enumerate(perm):
            r = base[pi][:]
            if flip >> i & 1:
                r = r[8:] + r[:8]
            rows.append(r)
        col = expand(rows, 4, 64)
        got = gs_model(host, w, h, col)
        m = sum(1 for a, b in zip(ref, got) if a == b)
        best.append((m, perm, flip))
best.sort(reverse=True)
print(best[:5])
