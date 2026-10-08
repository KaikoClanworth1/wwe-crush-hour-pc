# Hidden match benchmark: Exhibition -> RAW -> Rock -> match via auto input, then averages the real game
# frame rate (VIF1 frame kicks) over the last 30 seconds. Screenshot near the end in testrun\shots.
param([int]$Seconds = 200, [string]$Extra = "", [string]$Exe = "", [double]$ShotSec = 190)
$root = "D:\Xbox Games Ports\WWE Crush Hour"
$a = @()
for ($t = 10; $t -le 45; $t += 5) { $a += "${t}:START" }
for ($t = 72; $t -le 250; $t += 6) { $a += "${t}:CROSS" }
$ai = $a -join ","
$x = "PS2X_STATUS=1;PS2X_SYMBOLS=$root\recomp\symbols.txt"
if ($Extra) { $x = "$x;$Extra" }
& "$root\tools\bgtest.ps1" -Seconds $Seconds -ShotSec $ShotSec -AutoInput $ai -Extra $x -Exe $Exe | Out-Null
$l = Select-String -Path "$root\testrun\stdout.txt" -Pattern "\[frames\]" | Select -Last 15
$fps = ($l | ForEach-Object { if ($_.Line -match 'rate=([\d.]+)') { [double]$Matches[1] } } | Measure-Object -Average -Minimum -Maximum)
"match fps avg={0:N1} min={1} max={2} (last {3} x 2s)" -f $fps.Average, $fps.Minimum, $fps.Maximum, $fps.Count
if ($fps.Maximum -eq 0) { "HANG: no frames in the last 30 s" }
