# Runs the recompiled game hidden (no window, no focus steal) for N seconds, capturing logs + screenshots.
param(
    [int]$Seconds = 40,
    [string]$AutoInput = "",
    [double]$ShotSec = 4,
    [string]$Extra = "",
    [string]$Exe = ""
)
# PS2X_PORT_ROOT: the port's working tree (GameFiles, recomp, testrun); see ps2x_paths.py.
$root = if ($env:PS2X_PORT_ROOT) { $env:PS2X_PORT_ROOT } else { "D:\Xbox Games Ports\WWE Crush Hour" }
# This checkout (also when run through the dev tree's <root>\tools junction).
$repo = if (Test-Path "$PSScriptRoot\..\..\ps2xRuntime") { (Resolve-Path "$PSScriptRoot\..\..").Path } else { "$root\PS2Recomp" }
$exe = if ($Exe) { $Exe } else { "$repo\out\build\ps2xRuntime\Release\ps2EntryRunner.exe" }
$out = "$root\testrun"
# Only test builds (ps2EntryRunner*); never the player's WWECrushHour.exe.
Get-Process ps2EntryRunner* -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process ps2EntryRunner* -ErrorAction SilentlyContinue | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null

# Start from a clean slate: env vars set by a previous run in the same PowerShell session would leak in.
Get-ChildItem env: | Where-Object { $_.Name -like "PS2X_*" } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$env:PS2X_BACKGROUND = "1"
$env:PS2X_MUTE = "1"
$env:PS2X_SCREENSHOT_DIR = "$out\shots"
$env:PS2X_SCREENSHOT_SEC = "$ShotSec"
$env:PS2X_AUTOINPUT = $AutoInput
foreach ($kv in ($Extra -split ";")) { if ($kv -match "^(\w+)=(.*)$") { Set-Item "env:$($Matches[1])" $Matches[2] } }

$p = Start-Process -FilePath $exe -ArgumentList "`"$root\GameFiles\SLUS_203.85`"" -WorkingDirectory "$root\GameFiles" `
    -RedirectStandardOutput "$out\stdout.txt" -RedirectStandardError "$out\stderr.txt" -WindowStyle Hidden -PassThru
try { $p.PriorityClass = 'BelowNormal' } catch {}
$exited = $p.WaitForExit($Seconds * 1000)
if (-not $exited) { Stop-Process -Id $p.Id -Force; $p.WaitForExit(10000) | Out-Null }
"exited=$exited code=$(if ($exited) { $p.ExitCode } else { 'killed' })"
"shots: " + ((Get-ChildItem "$out\shots" -ErrorAction SilentlyContinue | Measure-Object).Count)
