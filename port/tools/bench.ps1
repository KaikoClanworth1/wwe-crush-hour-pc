# Hidden menu benchmark: boots to the main menu (START presses during the intro), then averages the
# real game frame rate (VIF1 frame kicks) over the last 30 seconds and flags hangs.
param([int]$Seconds = 110, [string]$Extra = "", [string]$Exe = "")
$root = "D:\Xbox Games Ports\WWE Crush Hour"
$ai = (10..45 | Where-Object { $_ % 5 -eq 0 } | ForEach-Object { "$($_):START" }) -join ","
$x = "PS2X_STATUS=1;PS2X_SYMBOLS=$root\recomp\symbols.txt"
if ($Extra) { $x = "$x;$Extra" }
& "$root\tools\bgtest.ps1" -Seconds $Seconds -ShotSec 1000 -AutoInput $ai -Extra $x -Exe $Exe | Out-Null
$l = Select-String -Path "$root\testrun\stdout.txt" -Pattern "\[frames\]" | Select -Last 15
$fps = ($l | ForEach-Object { if ($_.Line -match 'rate=([\d.]+)') { [double]$Matches[1] } } | Measure-Object -Average -Minimum -Maximum)
"menu fps avg={0:N1} min={1} max={2} (last {3} x 2s)" -f $fps.Average, $fps.Minimum, $fps.Maximum, $fps.Count
if ($fps.Maximum -eq 0) { "HANG: no frames in the last 30 s" }
