# Xbox button prompts for the PC port: the game's PS2 button art is replaced in two of its 512x256 textures (as the
# GS backend decodes them, RGBA with PS2 alpha 0..128), using the Xbox 360 glyphs of WWE SmackDown vs Raw 2011
# (menu/fontHD.pac font page as extracted by that port: Game Files/pad_icons/icons0_256x512.dds, 32-bit
# uncompressed, the 360 page in its top 512 rows).
#  - the menu font (button prompts "Proceed", "Return", ...): square / triangle / circle / cross -> X / Y / B / A
#  - Options > Controller Options: the face buttons on the pad picture and in the button list, and the shoulder
#    / stick labels "L2 button" -> "LT button" (L1 LB, R2 RT, R1 RB, R3 RS)
# Inputs: the decoded originals in tools/xbox_icons/<name>_<hash>.png (dumped with PS2X_ICON_LOG=<dir>; <hash> is
# gs_d3d11_backend.cpp's fnv1a over rows 122..159, columns 72..219, which identifies the texture at runtime).
# Output: PS2Recomp/ps2xRuntime/src/lib/gs/xbox_button_icons.inc (changed rectangles only) and a preview PNG.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
import glob, os, struct
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

# The button glyph sheet of WWE SmackDown vs Raw 2011 (PS2X_SVR2011_ICONS overrides the dev machine's copy).
SVR = os.environ.get('PS2X_SVR2011_ICONS', r'D:\Xbox Games Ports\WWE Smackdown Vs Raw 2011\Game Files\pad_icons\icons0_256x512.dds')
SRC_DIR = os.path.join(ROOT, 'tools', 'xbox_icons')
OUT = os.path.join(ROOT, r'PS2Recomp\ps2xRuntime\src\lib\gs\xbox_button_icons.inc')
FONT = r'C:\Windows\Fonts\arialbd.ttf'


def load_svr_page():
    d = open(SVR, 'rb').read()
    assert d[:4] == b'DDS ' and struct.unpack_from('<I', d, 88)[0] == 32
    h, w = struct.unpack_from('<II', d, 12)
    masks = struct.unpack_from('<IIII', d, 92)
    px = np.frombuffer(d, dtype=np.uint32, offset=128, count=w * h).reshape(h, w)
    ch = [((px & m) >> ((m & -m).bit_length() - 1)).astype(np.uint8) for m in masks]
    return np.dstack(ch)[:512]  # RGBA 0..255, the 360 page


PAGE = load_svr_page()
# Xbox face glyphs: 36x32 cells (a 32 px button + its shadow) in the 360 page.
GLYPH = {'A': (196, 5), 'B': (132, 4), 'X': (4, 4), 'Y': (68, 4)}


def glyph(name, width):
    """The glyph scaled to `width` px (premultiplied-alpha resize), RGBA float 0..1."""
    x, y = GLYPH[name]
    g = PAGE[y:y + 32, x:x + 36].astype(np.float32) / 255.0
    h = max(1, round(width * 32 / 36))
    pm = g.copy()
    pm[..., :3] *= pm[..., 3:4]
    im = Image.fromarray((pm * 255).astype(np.uint8), 'RGBA').resize((width, h), Image.LANCZOS)
    r = np.asarray(im).astype(np.float32) / 255.0
    a = r[..., 3:4]
    r[..., :3] = np.where(a > 1e-4, r[..., :3] / np.maximum(a, 1e-4), 0.0)
    return np.clip(r, 0, 1)


def paste_over(tex, g, cx, cy):
    """Alpha-composite glyph g (RGBA 0..1, its 32 px button centred at (cx, cy)) over the texture."""
    h, w = g.shape[:2]
    btn = w * 32 / 36  # button diameter at this scale
    x0 = int(round(cx - btn / 2))
    y0 = int(round(cy - btn / 2))
    region = tex[y0:y0 + h, x0:x0 + w].astype(np.float32)
    a = g[..., 3:4]
    region[..., :3] = region[..., :3] * (1 - a) + g[..., :3] * 255.0 * a
    # PS2 alpha: keep an opaque texel opaque; else source-over in 0..128 units.
    region[..., 3:4] = region[..., 3:4] + (128.0 - region[..., 3:4]) * a
    tex[y0:y0 + h, x0:x0 + w] = np.clip(np.round(region), 0, 255).astype(np.uint8)


def edit_font(tex):
    # One row of button glyphs: square, triangle, circle, cross in cells x 72..109 / 110..145 / 146..182 / 183..219,
    # y 122..159. Cleared, then the Xbox glyph centred in each cell (the 360 layout has X / Y / B / A there).
    cells = [(72, 109, 'X'), (110, 145, 'Y'), (146, 182, 'B'), (183, 219, 'A')]
    for x0, x1, name in cells:
        tex[122:160, x0:x1 + 1] = 0
        paste_over(tex, glyph(name, 36), (x0 + x1 + 1) / 2, 141)


def inpaint(tex, mask, iters=400):
    """Fill masked texels from their surroundings (Jacobi diffusion), RGB only."""
    img = tex[..., :3].astype(np.float32)
    m = mask.astype(bool)
    img[m] = img[~m].mean(axis=0)
    for _ in range(iters):
        nb = (np.roll(img, 1, 0) + np.roll(img, -1, 0) + np.roll(img, 1, 1) + np.roll(img, -1, 1)) / 4
        img[m] = nb[m]
    tex[..., :3][m] = np.clip(np.round(img[m]), 0, 255).astype(np.uint8)


def relabel(tex, box, letter):
    """In the white-on-black label inside `box`, replace its 2nd glyph (the digit) with `letter`, same style."""
    x0, y0, x1, y1 = box
    rgb = tex[..., :3].astype(int)
    fill = (rgb[..., 2] > rgb[..., 0] + 6) & (rgb.min(axis=2) > 105)
    sub = fill[y0:y1, x0:x1]
    cols = sub.any(axis=0)
    segs, start = [], None
    for x, v in enumerate(list(cols) + [False]):
        if v and start is None:
            start = x
        elif not v and start is not None:
            if segs and start - segs[-1][1] <= 2:
                segs[-1] = (segs[-1][0], x - 1)  # a glyph's parts (gap <= 1 px)
            else:
                segs.append((start, x - 1))
            start = None
    (fa, fb), (da, db) = segs[0], segs[1]
    rows = np.where(sub[:, fa:fb + 1].any(axis=1))[0]
    top, bot = y0 + rows.min(), y0 + rows.max()  # cap height of the first letter
    colour = np.median(rgb[y0:y1, x0:x1][sub], axis=0)
    gx0, gx1 = x0 + da, x0 + db
    # Clear the digit (with its outline), keeping the first letter's outline.
    mask = np.zeros(fill.shape, bool)
    mask[top - 2:bot + 3, gx0 - 1:gx1 + 3] = True
    inpaint(tex, mask)
    # The new letter at the same cap height, starting where the digit did: lavender fill, ~1.5 px black outline.
    cap = bot - top + 2  # the fill mask misses the antialiased top / bottom texels
    S = 8
    size = cap * S / 0.716  # Arial cap height = 0.716 em
    font = ImageFont.truetype(FONT, int(round(size)))
    C = 24  # canvas (texels)
    W, H = C * S, C * S
    glyph_im = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(glyph_im)
    bb = d.textbbox((0, 0), letter, font=font)
    d.text((-bb[0], 4 * S - bb[1]), letter, fill=255, font=font)
    # 85% wide: the letters are wider than the digits they replace; keeps a gap before "button".
    glyph_im = glyph_im.resize((int(W * 0.85), H), Image.LANCZOS)
    im = Image.new('L', (W, H), 0)
    im.paste(glyph_im, (4 * S, 0))
    fillm = im
    outl = im.filter(ImageFilter.MaxFilter(2 * int(2.0 * S) + 1))
    fa_ = np.asarray(fillm.resize((W // S, H // S), Image.LANCZOS)).astype(np.float32) / 255
    oa_ = np.asarray(outl.resize((W // S, H // S), Image.LANCZOS)).astype(np.float32) / 255
    narrow = gx1 - gx0 < 4  # a '1': the wider letter starts a texel earlier
    ox, oy = gx0 - 4 - (1 if narrow else 0), top - 5
    reg = tex[oy:oy + C, ox:ox + C, :3].astype(np.float32)
    fa_, oa_ = fa_[:reg.shape[0], :reg.shape[1]], oa_[:reg.shape[0], :reg.shape[1]]
    reg = reg * (1 - oa_[..., None]) + np.array([8, 8, 12], np.float32) * oa_[..., None]
    reg = reg * (1 - fa_[..., None]) + colour * fa_[..., None]
    tex[oy:oy + C, ox:ox + C, :3] = np.clip(np.round(reg), 0, 255).astype(np.uint8)


def edit_controller_options(tex):
    # Shoulder / stick labels.
    for box, letter in (((15, 36, 95, 56), 'T'), ((15, 72, 95, 94), 'B'), ((370, 42, 450, 62), 'T'),
                        ((370, 76, 450, 98), 'B'), ((178, 222, 245, 242), 'S')):
        relabel(tex, box, letter)
    # The button list at the right: triangle / circle / cross / square discs (29 px, overlapping, drawn in order).
    for name, cx, cy in (('Y', 377, 131), ('B', 377, 160), ('A', 377, 183), ('X', 377, 211)):
        paste_over(tex, glyph(name, 34), cx, cy)
    # The face buttons on the pad picture.
    for name, cx, cy in (('Y', 313.5, 132), ('X', 287, 151), ('B', 334, 153), ('A', 307, 169)):
        paste_over(tex, glyph(name, 19), cx, cy)


EDITS = {'font_menu': edit_font, 'controller_options': edit_controller_options}


def changed_rects(a, b, tile=16):
    """Rectangles (x, y, w, h) covering the changed texels, merged per 16-row band."""
    diff = (a != b).any(axis=2)
    rects = []
    for ty in range(0, diff.shape[0], tile):
        band = diff[ty:ty + tile]
        if not band.any():
            continue
        cols = np.where(band.any(axis=0))[0]
        start = prev = cols[0]
        for c in list(cols[1:]) + [None]:
            if c is not None and c - prev <= 8:
                prev = c
                continue
            rows = np.where(band[:, start:prev + 1].any(axis=1))[0]
            rects.append((int(start), ty + int(rows.min()), int(prev - start + 1), int(rows.max() - rows.min() + 1)))
            if c is not None:
                start = prev = c
    return rects


lines = ['// Generated by tools/make_xbox_icons.py: Xbox 360 button glyphs (from WWE SmackDown vs Raw 2011) patched into',
         '// the game\'s textures. Each patch: texture id (fnv1a of rows 122..159, columns 72..219 of the decoded 512x256',
         '// texture), rectangle, RGBA texels (PS2 alpha 0..128) as uint32 0xAABBGGRR.',
         'struct XboxIconPatch { uint64_t texture; uint16_t x, y, w, h; const uint32_t *texels; };']
patches = []
previews = []
for path in sorted(glob.glob(os.path.join(SRC_DIR, '*.png'))):
    base = os.path.basename(path)[:-4]
    if base.startswith('preview'):
        continue
    name, hsh = base.rsplit('_', 1)
    if name not in EDITS:
        continue
    orig = np.array(Image.open(path).convert('RGBA'))
    tex = orig.copy()
    EDITS[name](tex)
    for i, (x, y, w, h) in enumerate(changed_rects(orig, tex)):
        block = tex[y:y + h, x:x + w].astype(np.uint32)
        words = block[..., 0] | (block[..., 1] << 8) | (block[..., 2] << 16) | (block[..., 3] << 24)
        sym = 'kXboxIcon_%s_%d' % (name, i)
        lines.append('static const uint32_t %s[%d] = {%s};' % (sym, w * h, ','.join('0x%x' % v for v in words.ravel())))
        patches.append('{0x%sull, %d, %d, %d, %d, %s}' % (hsh, x, y, w, h, sym))
    vis = tex.copy()
    vis[..., 3] = np.minimum(255, vis[..., 3].astype(int) * 2)
    previews.append(Image.fromarray(vis, 'RGBA'))
lines.append('static const XboxIconPatch kXboxIconPatches[] = {%s};' % ', '.join(patches))
open(OUT, 'w', newline='\n').write('\n'.join(lines) + '\n')
sheet = Image.new('RGBA', (512, 256 * len(previews)), (80, 80, 80, 255))
for i, p in enumerate(previews):
    sheet.alpha_composite(p, (0, 256 * i))
sheet.save(os.path.join(SRC_DIR, 'preview.png'))
print('patches', len(patches), '->', OUT)
