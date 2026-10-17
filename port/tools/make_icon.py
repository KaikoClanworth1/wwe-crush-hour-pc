# Builds the PC icon from the game's own memory-card icon (BASLUS-20385/view.ico, a PS2 3D icon).
# Its texture (last 32 KiB: 128x128 RGB5A1, uncompressed for texture type 7) is the WWE scratch logo on
# black; black is keyed out to alpha. Output: PS2Recomp/ps2xRuntime/res/game.ico (16..256 px).
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps2x_paths import OUTPUT, ROOT, RUNTIME, TOOLS, out, root, runtime  # noqa: E402,F401
import struct
from PIL import Image

src = root('GameFiles', 'mc0', 'BASLUS-20385', 'view.ico')
dst = runtime('res', 'game.ico')

d = open(src, 'rb').read()
magic, shapes, textype = struct.unpack_from('<3I', d, 0)
assert magic == 0x10000 and textype & 8 == 0, 'expected an uncompressed PS2 icon texture'
tex = d[-128 * 128 * 2:]
px = []
for i in range(128 * 128):
    v = struct.unpack_from('<H', tex, i * 2)[0]
    r, g, b = (v & 31) << 3, ((v >> 5) & 31) << 3, ((v >> 10) & 31) << 3
    m = max(r, g, b)
    a = min(255, m * 3)  # black background -> transparent, soft edges
    if a:  # un-premultiply against black
        r, g, b = (min(255, c * 255 // a) for c in (r, g, b))
    px.append((r, g, b, a))
im = Image.new('RGBA', (128, 128))
im.putdata(px)
im = im.resize((256, 256), Image.LANCZOS)
im.save(dst, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
im.save(root('recomp', 'icon_preview.png'))
print('wrote', dst)
