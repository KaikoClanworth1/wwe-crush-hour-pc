# Hidden menu test: boots to the main menu, opens Exhibition -> arena chooser -> Match Type and stays there.
# Screenshots every 2 s; prints a per-shot noise score of the menu panel (garbage textures score high).
param([string]$Extra = "", [int]$Seconds = 120, [string]$Tag = "menu")
# PS2X_PORT_ROOT: the port's working tree (GameFiles, recomp, testrun); see ps2x_paths.py.
$root = if ($env:PS2X_PORT_ROOT) { $env:PS2X_PORT_ROOT } else { "D:\Xbox Games Ports\WWE Crush Hour" }
$a = @()
for ($t = 10; $t -le 80; $t += 5) { $a += "${t}:START" }
$a += "@1+2.0:CROSS"; $a += "@1+4.5:CROSS"
$a += "@3+1.5:CROSS"
$x = "PS2X_STATUS=1;PS2X_AUTOINPUT_ANCHOR=1d7470"
if ($Extra) { $x = "$x;$Extra" }
& "$PSScriptRoot\bgtest.ps1" -Seconds $Seconds -ShotSec 2 -AutoInput ($a -join ",") -Extra $x | Out-Null
python -c @"
from PIL import Image, ImageChops, ImageStat
import glob
fs = sorted(glob.glob(r'$root\testrun\shots\*.bmp'))[30:]
bad = 0
scores = []
for f in fs:
    im = Image.open(f).convert('L').crop((640, 200, 1280, 700))
    sh = im.transform(im.size, Image.AFFINE, (1, 0, 1, 0, 1, 0))
    s = ImageStat.Stat(ImageChops.difference(im, sh)).mean[0]
    scores.append(s)
    bad += s > 40
print('$Tag', 'shots', len(fs), 'noisy', bad, 'max', round(max(scores) if scores else 0, 1), 'median', round(sorted(scores)[len(scores)//2] if scores else 0, 1))
"@
