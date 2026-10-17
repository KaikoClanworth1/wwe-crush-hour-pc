# Hidden collision drive test: the matchbench route into an Exhibition match, then accelerate (Cross held)
# with optional steering, screenshots every 2 s. Montage -> $Out (compare runs for driving through walls).
param([string]$Exe = "", [string]$Extra = "", [string]$Out = "", [string]$Steer = "", [int]$Seconds = 230, [int]$Arena = 0, [double]$ShotSec = 2)
# PS2X_PORT_ROOT: the port's working tree (GameFiles, recomp, testrun); see ps2x_paths.py.
$root = if ($env:PS2X_PORT_ROOT) { $env:PS2X_PORT_ROOT } else { "D:\Xbox Games Ports\WWE Crush Hour" }
$a = @()
for ($t = 10; $t -le 60; $t += 5) { $a += "${t}:START" }  # to 60 s: a loaded PC can reach the title after 45 s
# Menu timing drifts 3-12 s with machine load, so everything after the START presses is anchored to calls of
# Shell::OnSetOnScreenPrompt_Tag (0x1d7470): PS2X_AUTOINPUT "@n+t:BUTTON" = t s after its n-th call.
# Calls 1-2 = main menu, 3-5 = arena chooser (Change Arena), 6-7 = match-type chooser, 8-10 = character select.
# -Arena N presses RIGHT N times in the chooser (from RAW: 1 = Hell in the Cell, 2 = Hardcore), then Cross.
$a += "@1+2.0:CROSS"; $a += "@1+4.5:CROSS"   # twice: the sign is still flying in at +2 s on slow runs
for ($i = 0; $i -lt $Arena; $i++) { $a += "@3+{0}:RIGHT" -f (1.0 + $i * 0.7) }
$a += "@3+{0}:CROSS" -f (1.0 + $Arena * 0.7 + 0.5)
$a += "@6+1.5:CROSS"; $a += "@6+3.5:CROSS"
foreach ($t in 2.0, 4.0, 6.0, 8.0) { $a += "@8+${t}:CROSS" }
$a += "155-$($Seconds):CROSS"
if ($Steer) { $a += $Steer }
$x = "PS2X_STATUS=1"
$x = "$x;PS2X_AUTOINPUT_ANCHOR=1d7470"
if ($Extra) { $x = "$x;$Extra" }
& "$PSScriptRoot\bgtest.ps1" -Seconds $Seconds -ShotSec $ShotSec -AutoInput ($a -join ",") -Extra $x -Exe $Exe | Out-Null
if ($Out) {
    python -c @"
from PIL import Image
import glob
fs = sorted(glob.glob(r'$root\testrun\shots\*.bmp'))[74:]
fs = fs[:36]
W, H = 320, 180
m = Image.new('RGB', (W * 6, H * ((len(fs) + 5) // 6)))
for i, f in enumerate(fs):
    try:
        m.paste(Image.open(f).convert('RGB').resize((W, H)), ((i % 6) * W, (i // 6) * H))
    except OSError:
        pass
m.save(r'$Out')
print('frames', len(fs))
"@
}
