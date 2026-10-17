# The restored ladder arena's tile for the arena chooser (mod_menu.cpp: the "Ladder Arena" mod): a 512x256 tile in
# the style of the game's 13 arena tiles, drawn by the GS backend over the Survivor Series tile's texture while the
# chooser shows the ladder arena.
#  - frame, rivets and WWE logo: the tiles' common pixels (each tile's label masked out, averaged over the tiles)
#  - picture: a screenshot of the ladder arena (tools/xbox_icons/ladder_shot.png; without one, the donor's picture,
#    darkened), fitted to the tiles' picture area, with the screen's edge shading
#  - label "Ladder": the game's own letters (L from the Lumberjack tile, a d d e r from the Hardcore tile), redrawn
#    with the labels' outline and drop shadow, right-aligned on the Survivor Series label's baseline
# Inputs: tools/xbox_icons/tile_<level>.png, the 13 tiles as the GS backend decodes them (PS2X_RASTER_DUMP=<dir>
# while the arena chooser cycles through them). The donor is identified at run time like the other texture patches:
# gs_d3d11_backend.cpp's fnv1a over rows 122..159, columns 72..219.
# Output: PS2Recomp/ps2xRuntime/src/lib/gs/ladder_tile.inc (palettized rectangle) and tools/xbox_icons/
# ladder_tile_preview.png.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
import glob, os
from collections import deque
import numpy as np
from PIL import Image, ImageFilter

SRC = os.path.join(ROOT, 'tools', 'xbox_icons')
OUT = os.path.join(ROOT, r'PS2Recomp\ps2xRuntime\src\lib\gs\ladder_tile.inc')
DONOR = 'survivor_series'
LEVELS = ['raw', 'hell_inthe_cell', 'hardcore_arena', 'running_gauntlet', 'cage_match', 'art_test', 'battle_royal',
          'ironman', 'kotr', 'lumberjack', 'royal_rumble', 'smack_down', 'survivor_series']


def load(name):
    return np.array(Image.open(os.path.join(SRC, 'tile_%s.png' % name)).convert('RGBA')).astype(np.float32)


def fnv1a(data, h):
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def dilate(m, r):
    out = m.copy()
    H, W = m.shape
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            if dx * dx + dy * dy > r * r + r:
                continue
            sh = np.zeros_like(m)
            sh[max(0, dy):H + min(0, dy), max(0, dx):W + min(0, dx)] = m[max(0, -dy):H - max(0, dy), max(0, -dx):W - max(0, dx)]
            out = np.maximum(out, sh)
    return out


def shift(m, dx, dy):
    out = np.zeros_like(m)
    H, W = m.shape
    out[dy:, dx:] = m[:H - dy, :W - dx]
    return out


def label_mask(a):
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    blue = (b > r + 40) & (b > g + 25) & (b > 100)
    blue[:200] = False
    return blue


def components(mask):
    H, W = mask.shape
    lab = np.zeros((H, W), int)
    out = []
    for y in range(H):
        for x in range(W):
            if mask[y, x] and not lab[y, x]:
                n = len(out) + 1
                q = deque([(y, x)])
                lab[y, x] = n
                pts = []
                while q:
                    cy, cx = q.popleft()
                    pts.append((cy, cx))
                    for dy in (-1, 0, 1):
                        for dx in (-1, 0, 1):
                            ny, nx = cy + dy, cx + dx
                            if 0 <= ny < H and 0 <= nx < W and mask[ny, nx] and not lab[ny, nx]:
                                lab[ny, nx] = n
                                q.append((ny, nx))
                ys = [p[0] for p in pts]
                xs = [p[1] for p in pts]
                out.append((min(xs), min(ys), max(xs) + 1, max(ys) + 1, n))
    return sorted(out), lab


tiles = {n: load(n) for n in LEVELS}
donor = tiles[DONOR]
h = 1469598103934665603
for y in range(122, 160):
    h = fnv1a(donor[y, 72:220].astype(np.uint8).tobytes(), h)

# Frame: each tile's label (fill + outline + shadow) masked out, the rest averaged.
stack = np.stack([tiles[n] for n in LEVELS])
covered = np.stack([dilate(label_mask(tiles[n]).astype(np.float32), 5) > 0 for n in LEVELS])
w = (~covered).astype(np.float32)[..., None]
base = (stack * w).sum(0) / np.maximum(w.sum(0), 1e-3)
base[w.sum(0)[..., 0] == 0] = donor[w.sum(0)[..., 0] == 0]

# Picture area: the pixels that differ between the tiles (the region holding the centre, holes filled); the logo's
# opaque pixels (alike in every tile) stay frame.
cnt = w.sum(0)
mean = (stack * w).sum(0) / np.maximum(cnt, 1e-3)
sd = np.sqrt((((stack - mean) ** 2) * w).sum(0) / np.maximum(cnt, 1e-3))[..., :3].max(axis=2)
var = (sd > 14) & (cnt[..., 0] >= 3)
seen = np.zeros_like(var)
q = deque([(128, 256)])
seen[128, 256] = True
while q:
    y, x = q.popleft()
    for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        ny, nx = y + dy, x + dx
        if 0 <= ny < 256 and 0 <= nx < 512 and var[ny, nx] and not seen[ny, nx]:
            seen[ny, nx] = True
            q.append((ny, nx))
ys, xs = np.where(seen)
px0, py0, px1, py1 = xs.min(), ys.min(), xs.max() + 1, ys.max() + 1
area = np.zeros_like(var)
for y in range(py0, py1):  # fill holes row by row inside the region's span
    row = np.where(seen[y])[0]
    if len(row):
        area[y, row.min():row.max() + 1] = True
logo = area & ~var & (np.arange(512)[None, :] < 90) & (np.arange(256)[:, None] < 70)
picture = area & ~logo

shot_path = os.path.join(SRC, 'ladder_shot.png')
pw, ph = px1 - px0, py1 - py0
if os.path.exists(shot_path):
    shot = Image.open(shot_path).convert('RGB')
    sw, sh_ = shot.size
    s = max(pw / sw, ph / sh_)
    shot = shot.resize((max(pw, int(round(sw * s))), max(ph, int(round(sh_ * s)))), Image.LANCZOS)
    ox, oy = (shot.size[0] - pw) // 2, (shot.size[1] - ph) // 2
    pic = np.array(shot.crop((ox, oy, ox + pw, oy + ph))).astype(np.float32)
else:  # placeholder: the averaged pictures, dark
    pic = base[py0:py1, px0:px1, :3] * 0.6
# The screen's edge shading (the tiles' pictures darken towards the frame).
yy, xx = np.mgrid[0:ph, 0:pw]
edge = np.minimum(np.minimum(xx, pw - 1 - xx) / 14.0, np.minimum(yy, ph - 1 - yy) / 10.0)
pic *= (0.72 + 0.28 * np.clip(edge, 0, 1))[..., None]
out = base.copy()
sub = out[py0:py1, px0:px1, :3]
pm = picture[py0:py1, px0:px1]
sub[pm] = pic[pm]

# Label "Ladder".
hard = tiles['hardcore_arena']
lumb = tiles['lumberjack']
hc, hlab = components(label_mask(hard))
lc, llab = components(label_mask(lumb))
hc = [c for c in hc if (c[2] - c[0]) * (c[3] - c[1]) > 30]
lc = [c for c in lc if (c[2] - c[0]) * (c[3] - c[1]) > 30]
H_ = dict(zip('Hardcore', range(8)))
glyphs = [(lumb, llab, lc[0], 243),                        # L
          (hard, hlab, hc[1], 243),                        # a
          (hard, hlab, hc[3], 243),                        # d
          (hard, hlab, hc[3], 243),                        # d
          (hard, hlab, hc[7], 243),                        # e
          (hard, hlab, hc[2], 243)]                        # r
gap = 3
total = sum(g[2][2] - g[2][0] for g in glyphs) + gap * (len(glyphs) - 1)
right, baseline = 490, 244
x = right - total
fill = np.zeros((256, 512), np.float32)
col = np.zeros((256, 512, 3), np.float32)
for src, lab, (x0, y0, x1, y1, n), srcBase in glyphs:
    m = lab[y0:y1, x0:x1] == n
    dy = baseline - srcBase
    fill[y0 + dy:y1 + dy, x:x + (x1 - x0)][m] = 1.0
    col[y0 + dy:y1 + dy, x:x + (x1 - x0)][m] = src[y0:y1, x0:x1, :3][m]
    x += (x1 - x0) + gap
outline = dilate(fill, 2)
shadow = shift(outline, 2, 2) * 0.6
rgb = out[..., :3]
rgb[:] = rgb * (1 - shadow[..., None])
rgb[:] = rgb * (1 - outline[..., None]) + np.array([12, 10, 18], np.float32) * outline[..., None]
rgb[fill > 0] = col[fill > 0]
out[..., 3] = 128

# The patch rectangle: the picture area and the label.
ys, xs = np.where(picture | (dilate(fill, 5) > 0))
rx0, ry0, rx1, ry1 = xs.min(), ys.min(), xs.max() + 1, ys.max() + 1
rect = np.clip(out[ry0:ry1, rx0:rx1] + 0.5, 0, 255).astype(np.uint8)
q = Image.fromarray(rect[..., :3]).quantize(256, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG)
pal = np.array(q.getpalette()[:768]).reshape(-1, 3)
idx = np.array(q)
words = [int(r) | (int(g) << 8) | (int(b) << 16) | (128 << 24) for r, g, b in pal]
words += [128 << 24] * (256 - len(words))
W_, H2 = rx1 - rx0, ry1 - ry0
lines = ['// Generated by port/tools/make_ladder_tile.py: the restored ladder arena\'s tile in the arena chooser, drawn',
         '// over the Survivor Series tile (texture hash below) while the chooser shows the ladder arena. 256-colour',
         '// palette (RGBA, PS2 alpha 0..128, as uint32 0xAABBGGRR) and one index per texel of the rectangle.',
         'struct LadderTilePatch { uint64_t texture; uint16_t x, y, w, h; const uint32_t *palette; const uint8_t *indices; };',
         'static const uint32_t kLadderTilePalette[256] = {%s};' % ','.join('0x%x' % v for v in words)]
flat = idx.ravel()
chunks = [','.join(str(int(v)) for v in flat[i:i + 64]) for i in range(0, len(flat), 64)]
lines.append('static const uint8_t kLadderTileIndices[%d] = {\n%s};' % (len(flat), ',\n'.join(chunks)))
lines.append('static const LadderTilePatch kLadderTile = {0x%016xull, %d, %d, %d, %d, kLadderTilePalette, kLadderTileIndices};'
             % (h, rx0, ry0, W_, H2))
open(OUT, 'w', newline='\n').write('\n'.join(lines) + '\n')
prev = donor.copy()
prev[ry0:ry1, rx0:rx1, :3] = pal[idx]
prev[..., 3] = 255
donor = donor.copy()
donor[..., 3] = 255
Image.fromarray(np.concatenate([donor, prev], 1).astype(np.uint8)).save(os.path.join(SRC, 'ladder_tile_preview.png'))
print('donor %016x rect %d,%d %dx%d picture %d,%d-%d,%d shot %s' % (h, rx0, ry0, W_, H2, px0, py0, px1, py1,
                                                                    os.path.exists(shot_path)))
