# Counts half-drawn frames in testrun\shots: a shot whose mean brightness dips well below both neighbours
# (the menu sky / HUD missing). Usage: python tools\flickercount.py [first_index] [montage.png]
# The montage shows each dip between its neighbours.
import glob, sys
from PIL import Image, ImageStat

first = int(sys.argv[1]) if len(sys.argv) > 1 else 0
out = sys.argv[2] if len(sys.argv) > 2 else None
fs, lum = [], []
for f in sorted(glob.glob(r'D:\Xbox Games Ports\WWE Crush Hour\testrun\shots\*.bmp'))[first:]:
    try:
        lum.append(ImageStat.Stat(Image.open(f).convert('L').resize((96, 54))).mean[0])
        fs.append(f)
    except OSError:  # last shot cut off when the run was killed
        break
dips = [i for i in range(1, len(lum) - 1) if lum[i] < 0.75 * min(lum[i - 1], lum[i + 1])]
print('shots=%d dips=%d %s' % (len(lum), len(dips), [i + first for i in dips]))
if out and dips:
    W, H = 480, 270
    m = Image.new('RGB', (W * 3, H * len(dips)))
    for r, i in enumerate(dips):
        for c, j in enumerate((i - 1, i, i + 1)):
            m.paste(Image.open(fs[j]).convert('RGB').resize((W, H)), (c * W, r * H))
    m.save(out)
