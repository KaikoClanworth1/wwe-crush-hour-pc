import importlib.util, sys
spec = importlib.util.spec_from_file_location("sc", "tools/swizzle_check.py")
BLOCK32 = [[0,1,4,5,16,17,20,21],[2,3,6,7,18,19,22,23],[8,9,12,13,24,25,28,29],[10,11,14,15,26,27,30,31]]
COL32 = [[0,1,4,5,8,9,12,13],[2,3,6,7,10,11,14,15],[16,17,20,21,24,25,28,29],[18,19,22,23,26,27,30,31],
         [32,33,36,37,40,41,44,45],[34,35,38,39,42,43,46,47],[48,49,52,53,56,57,60,61],[50,51,54,55,58,59,62,63]]
def addr32(x, y, bw):
    page = (y // 32) * bw + (x // 64)
    blk = BLOCK32[(y % 32) // 8][(x % 64) // 8]
    return page * 8192 + blk * 256 + COL32[y % 8][x % 8] * 4
w = h = 128
uw = w // 2
tbl = [[0]*16 for _ in range(16)]
blocks = {}
for y in range(h):
    for x in range(w):
        block_location = (y & ~0xF) * w + (x & ~0xF) * 2
        swap_selector = (((y + 2) >> 2) & 0x1) * 4
        posY = (((y & ~3) >> 1) + (y & 1)) & 0x7
        column_location = posY * w * 2 + ((x + swap_selector) & 0x7) * 4
        byte_num = ((y >> 1) & 1) + ((x >> 2) & 2)
        s = block_location + column_location + byte_num
        cx, cy = (s // 4) % uw, (s // 4) // uw
        A = addr32(cx, cy, 1) + s % 4
        blocks.setdefault(((y // 16), (x // 16)), set()).add(A // 256)
        if x < 16 and y < 16:
            tbl[y][x] = A % 256
for r in tbl:
    print(' '.join(f'{v:3d}' for v in r))
print('block ids for first block rows:', [sorted(blocks[(0, bx)]) for bx in range(8)])
