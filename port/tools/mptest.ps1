# Hidden split-screen test: main menu -> Multiplayer, then the scripted presses in -Steps (pad 2 buttons as
# "CROSS#2"), screenshots every -ShotSec. Anchored like drivetest: "@1+t" = t s after the main menu's first
# Shell::OnSetOnScreenPrompt_Tag call. -Out writes a montage of the shots from -From on.
param([string]$Steps = "", [string]$Extra = "", [string]$Out = "", [int]$Seconds = 150, [double]$ShotSec = 1, [int]$From = 40)
$root = "D:\Xbox Games Ports\WWE Crush Hour"
$a = @()
for ($t = 10; $t -le 80; $t += 5) { $a += "${t}:START" }  # to 80 s: slow boots under load
# Multiplayer is the 4th item of the main menu.
$a += "@1+4.0:DOWN"; $a += "@1+4.6:DOWN"; $a += "@1+5.2:DOWN"; $a += "@1+6.0:CROSS"
if ($Steps) { $a += $Steps }
$x = "PS2X_STATUS=1;PS2X_AUTOINPUT_ANCHOR=1d7470"
if ($Extra) { $x = "$x;$Extra" }
& "$root\tools\bgtest.ps1" -Seconds $Seconds -ShotSec $ShotSec -AutoInput ($a -join ",") -Extra $x | Out-Null
if ($Out) {
    python -c @"
from PIL import Image
import glob
fs = sorted(glob.glob(r'$root\testrun\shots\*.bmp'))[$($From):]
W, H = 240, 135
m = Image.new('RGB', (W * 10, H * ((len(fs) + 9) // 10)))
for i, f in enumerate(fs):
    try:
        m.paste(Image.open(f).convert('RGB').resize((W, H)), ((i % 10) * W, (i // 10) * H))
    except OSError:
        pass
m.save(r'$Out')
print('frames', len(fs))
"@
}
