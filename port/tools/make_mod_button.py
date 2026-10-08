# The mod menu's "m" plank on the main menu sign (mod_menu.cpp): built from the sign texture itself, as the GS backend
# decodes it (512x512, RGBA with PS2 alpha), so it matches the other planks exactly.
#  - body: the right end ("tab") of the "options" plank, mirrored for the left end, and a text-free strip of its
#    body stretched in between
#  - letter: the "m" of the "multiplayer" plank (the game's own menu lettering), lifted with its outline
#  - placed right of "options" on the sign, scaled to 88 % so it fits the board
#  - highlighted variant: frame in the game's highlight blue, letter lavender (the look of the game's selected items)
# Input: tools/xbox_icons/menu_sign_<hash>.png (dumped with PS2X_ICON_LOG=<dir>: icon_512x512_<hash>.rgba; <hash>
# is gs_d3d11_backend.cpp's fnv1a over rows 403..471, columns 112..391, which identifies the texture at runtime).
# Output: PS2Recomp/ps2xRuntime/src/lib/gs/mod_button.inc (both variants' rectangle) and tools/xbox_icons/
# mod_button_preview.png.
import glob, os
from collections import deque
import numpy as np
from PIL import Image

ROOT = r'D:\Xbox Games Ports\WWE Crush Hour'
SRC = glob.glob(os.path.join(ROOT, 'tools', 'xbox_icons', 'menu_sign_*.png'))
assert len(SRC) == 1, SRC
HASH = os.path.basename(SRC[0])[len('menu_sign_'):-4]
OUT = os.path.join(ROOT, r'PS2Recomp\ps2xRuntime\src\lib\gs\mod_button.inc')

a = np.array(Image.open(SRC[0]).convert('RGBA'))
RY0, RY1 = 403, 472            # the "options" plank's rows (frame, outline, shadow)
DY = 72                        # "multiplayer" row -> "options" row (same letter baseline)
X0, SCALE = 397, 0.88          # placement on the sign
rgb = a[..., :3].astype(np.float32)
red = (a[..., 0].astype(int) > a[..., 1].astype(int) + 12) & (a[..., 0].astype(int) > a[..., 2].astype(int) + 8)

tab = rgb[RY0:RY1, 370:390]    # right end of "options" (after its last letter)
strip = rgb[RY0:RY1, 367:371]  # text-free body
GL0, GL1 = 68, 126             # the "m" of "multiplayer" with its outline
glyph = rgb[RY0 - DY:RY1 - DY, GL0:GL1]
gred = red[RY0 - DY:RY1 - DY, GL0:GL1]
H, Wg = RY1 - RY0, GL1 - GL0
Wb = Wg + 8
body = np.asarray(Image.fromarray(strip.astype(np.uint8)).resize((Wb, H), Image.BILINEAR)).astype(np.float32)
gm = gred.copy()               # letter + its 2 px outline
for _ in range(2):
    d = gm.copy(); d[1:] |= gm[:-1]; d[:-1] |= gm[1:]; d[:, 1:] |= gm[:, :-1]; d[:, :-1] |= gm[:, 1:]; gm = d
ox = 4
body[:, ox:ox + Wg][gm] = glyph[gm]
pad = np.zeros((H, 20), bool)
inner = np.zeros((H, Wb), bool); inner[:, ox:ox + Wg] = gm
text = np.zeros((H, Wb), bool); text[:, ox:ox + Wg] = gred
plank = np.concatenate([tab[:, ::-1], body, tab], axis=1)
gmask = np.concatenate([pad, inner, pad], axis=1)
gtext = np.concatenate([pad, text, pad], axis=1)
W = plank.shape[1]
lum = plank.max(axis=2)
dark = lum <= 18               # the planks' outline (this texture is dark: outline 8..16, body 24..41)


def flood_outside(blocked):
    out = np.zeros(blocked.shape, bool)
    q = deque()
    for y in range(H):
        for x in (0, W - 1):
            if not blocked[y, x] and not out[y, x]:
                out[y, x] = True; q.append((y, x))
    for x in range(W):
        for y in (0, H - 1):
            if not blocked[y, x] and not out[y, x]:
                out[y, x] = True; q.append((y, x))
    while q:
        y, x = q.popleft()
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            ny, nx = y + dy, x + dx
            if 0 <= ny < H and 0 <= nx < W and not out[ny, nx] and not blocked[ny, nx]:
                out[ny, nx] = True; q.append((ny, nx))
    return out


mask = (~flood_outside(dark)).astype(np.float32)
ring = np.zeros_like(mask)
ring[1:] = np.maximum(ring[1:], mask[:-1]); ring[:-1] = np.maximum(ring[:-1], mask[1:])
ring[:, 1:] = np.maximum(ring[:, 1:], mask[:, :-1]); ring[:, :-1] = np.maximum(ring[:, :-1], mask[:, 1:])
alpha = np.maximum(mask, ring * 0.45)

# The frame: the large outline component (not the letter's outline, not rivets).
lab = np.zeros((H, W), int); sizes = {}; n = 0
for y in range(H):
    for x in range(W):
        if dark[y, x] and not gmask[y, x] and mask[y, x] > 0 and lab[y, x] == 0:
            n += 1; q = deque([(y, x)]); lab[y, x] = n; cnt = 0
            while q:
                cy, cx = q.popleft(); cnt += 1
                for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (1, -1), (-1, 1), (-1, -1)):
                    ny, nx = cy + dy, cx + dx
                    if 0 <= ny < H and 0 <= nx < W and dark[ny, nx] and not gmask[ny, nx] and lab[ny, nx] == 0:
                        lab[ny, nx] = n; q.append((ny, nx))
            sizes[n] = cnt
frame = np.isin(lab, [k for k, c in sizes.items() if c > 60])
hl = plank.copy()
v = plank.mean(axis=2)
hl[gtext] = np.clip(np.array([150, 158, 240]) * (0.55 + v[gtext, None] / 90.0), 0, 255)
hl[frame] = np.array([40, 52, 150]) * (0.9 + lum[frame, None] / 40.0)

Wn = int(round(W * SCALE))
al = np.asarray(Image.fromarray((alpha * 255).astype(np.uint8)).resize((Wn, H), Image.LANCZOS)).astype(np.float32) / 255


def place(src):
    o = a.copy()
    sc = np.asarray(Image.fromarray(np.clip(src, 0, 255).astype(np.uint8)).resize((Wn, H), Image.LANCZOS)).astype(np.float32)
    t = o[RY0:RY1, X0:X0 + Wn, :3].astype(np.float32)
    o[RY0:RY1, X0:X0 + Wn, :3] = np.clip(t * (1 - al[..., None]) + sc * al[..., None] + 0.5, 0, 255).astype(np.uint8)
    return o


variants = [place(plank), place(hl)]
lines = ['// Generated by port/tools/make_mod_button.py: the mod menu\'s "m" plank on the main menu sign, normal (variant 0)',
         '// and highlighted (variant 1). Rectangle texels: RGBA (PS2 alpha 0..128) as uint32 0xAABBGGRR.',
         'struct ModButtonPatch { uint64_t texture; uint8_t variant; uint16_t x, y, w, h; const uint32_t *texels; };']
entries = []
for i, o in enumerate(variants):
    blk = o[RY0:RY1, X0:X0 + Wn].astype(np.uint32)
    words = blk[..., 0] | (blk[..., 1] << 8) | (blk[..., 2] << 16) | (blk[..., 3] << 24)
    lines.append('static const uint32_t kModButton%d[%d] = {%s};' % (i, Wn * H, ','.join('0x%x' % w for w in words.ravel())))
    entries.append('{0x%sull, %d, %d, %d, %d, %d, kModButton%d}' % (HASH, i, X0, RY0, Wn, H, i))
lines.append('static const ModButtonPatch kModButtonPatches[] = {%s};' % ', '.join(entries))
open(OUT, 'w', newline='\n').write('\n'.join(lines) + '\n')
prev = np.concatenate([v[380:490, 300:512] for v in variants], axis=0).copy()
prev[..., 3] = 255
Image.fromarray(prev).resize((212 * 3, 220 * 3), Image.NEAREST).save(os.path.join(ROOT, 'tools', 'xbox_icons', 'mod_button_preview.png'))
print('texture', HASH, 'rect', X0, RY0, Wn, H, '->', OUT)
