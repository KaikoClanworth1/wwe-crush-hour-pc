# Regression run: plays the hidden scripted scenarios and compares each one's screenshots and log with a saved
# baseline (compare_shots.py). Run with -Baseline once on a known-good build, then without it after a change.
#   .\regress.ps1 -Baseline             # record <root>\regress\baseline
#   .\regress.ps1                        # run, compare, exit 1 on any regression
#   .\regress.ps1 -Only menu,drive       # a subset
param([switch]$Baseline, [string[]]$Only = @(), [string]$Exe = "", [double]$Threshold = 12)
# PS2X_PORT_ROOT: the port's working tree (GameFiles, recomp, testrun); see ps2x_paths.py.
$root = if ($env:PS2X_PORT_ROOT) { $env:PS2X_PORT_ROOT } else { "D:\Xbox Games Ports\WWE Crush Hour" }
$dest = if ($Baseline) { "$root\regress\baseline" } else { "$root\regress\last" }

# Each scenario runs one of the existing scripts (which all write <root>\testrun); the menu and split-screen runs
# are fixed menu paths, the drive runs a match on the default arena.
$scenarios = [ordered]@{
    menu  = { & "$PSScriptRoot\menutest.ps1" -Tag regress }
    drive = { & "$PSScriptRoot\drivetest.ps1" -Exe $Exe }
    mp    = { & "$PSScriptRoot\mptest.ps1" }
}

$failed = @()
foreach ($name in $scenarios.Keys) {
    if ($Only.Count -and $Only -notcontains $name) { continue }
    "== $name"
    & $scenarios[$name] | Out-Host
    $target = "$dest\$name"
    if (Test-Path $target) { Remove-Item -Recurse -Force $target }
    New-Item -ItemType Directory -Force $target | Out-Null
    Copy-Item -Recurse "$root\testrun\*" $target
    if ($Baseline) { "saved baseline $target"; continue }
    if (-not (Test-Path "$root\regress\baseline\$name")) { "no baseline for $name (run with -Baseline first)"; $failed += $name; continue }
    python "$PSScriptRoot\compare_shots.py" "$root\regress\baseline\$name" $target --threshold $Threshold | Out-Host
    if ($LASTEXITCODE -ne 0) { $failed += $name }
}
if ($Baseline) { exit 0 }
if ($failed.Count) { "REGRESSIONS: $($failed -join ', ')"; exit 1 }
"all scenarios match the baseline"
