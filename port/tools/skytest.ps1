# Hidden in-match flicker capture: drivetest route to arena N (default 1), screenshots every 0.5 s from
# the match start, then counts frames where the top band (sky) suddenly goes dark.
param([int]$Arena = 1, [string]$Extra = "", [int]$Seconds = 190)
# PS2X_PORT_ROOT: the port's working tree (GameFiles, recomp, testrun); see ps2x_paths.py.
$root = if ($env:PS2X_PORT_ROOT) { $env:PS2X_PORT_ROOT } else { "D:\Xbox Games Ports\WWE Crush Hour" }
$a = @()
for ($t = 10; $t -le 45; $t += 5) { $a += "${t}:START" }
for ($t = 72; $t -le 150; $t += 6) { if ($t -ne 78) { $a += "${t}:CROSS" } }
for ($i = 0; $i -lt $Arena; $i++) { $a += "{0}:RIGHT" -f (76.0 + $i * 0.7) }
$a += "{0}:CROSS" -f (76.0 + $Arena * 0.7 + 0.5)
$x = "PS2X_STATUS=1"
if ($Extra) { $x = "$x;$Extra" }
& "$PSScriptRoot\bgtest.ps1" -Seconds $Seconds -ShotSec 0.5 -AutoInput ($a -join ",") -Extra $x | Out-Null
python -c @"
import glob
from PIL import Image, ImageStat
fs = sorted(glob.glob(r'$root\testrun\shots\*.bmp'))[300:]
top = []
for f in fs:
    try:
        im = Image.open(f).convert('L').resize((96, 54))
        top.append(ImageStat.Stat(im.crop((15, 0, 81, 12))).mean[0])
    except OSError:
        break
dips = [i + 300 for i in range(1, len(top) - 1) if top[i] < 0.6 * min(top[i - 1], top[i + 1]) and min(top[i-1], top[i+1]) > 25]
print('frames=%d skydips=%d %s' % (len(top), len(dips), dips[:12]))
"@
