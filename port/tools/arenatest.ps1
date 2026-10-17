# Run tools\drivetest.ps1 until the wanted arena loads (its sound bank name, e.g. HELL_INT, HARDCORE,
# SURVIVOR), trying -Arena 1..4 because the menu timing varies. Leaves testrun\ from the matching run.
param([string]$Bank = "HELL_INT", [int]$Seconds = 215, [string]$Extra = "", [string]$Out = "", [int]$Tries = 6)
# PS2X_PORT_ROOT: the port's working tree (GameFiles, recomp, testrun); see ps2x_paths.py.
$root = if ($env:PS2X_PORT_ROOT) { $env:PS2X_PORT_ROOT } else { "D:\Xbox Games Ports\WWE Crush Hour" }
for ($i = 0; $i -lt $Tries; $i++) {
    $n = ($i % 4) + 1
    & "$PSScriptRoot\drivetest.ps1" -Arena $n -Seconds $Seconds -Extra $Extra -Out $Out | Out-Null
    $m = Select-String "$root\testrun\stdout.txt" -Pattern '\[seng\] loaded bank 1 (\w+)' | Select -Last 1
    $got = if ($m) { $m.Matches[0].Groups[1].Value } else { "?" }
    "try $($i + 1): -Arena $n -> $got"
    if ($got -like "$Bank*") { return }
}
"wanted $Bank not reached"
