# Builds the runner gently: Idle priority, limited to a few cores (child cl.exe processes inherit both).
param([int]$Cores = 6, [string]$Target = "ps2EntryRunner")
# PS2X_PORT_ROOT: the port's working tree (GameFiles, recomp, testrun); see ps2x_paths.py.
$root = if ($env:PS2X_PORT_ROOT) { $env:PS2X_PORT_ROOT } else { "D:\Xbox Games Ports\WWE Crush Hour" }
# This checkout (also when run through the dev tree's <root>\tools junction).
$repo = if (Test-Path "$PSScriptRoot\..\..\ps2xRuntime") { (Resolve-Path "$PSScriptRoot\..\..").Path } else { "$root\PS2Recomp" }
$log = "$root\recomp\build_log.txt"
$mask = [int64]([math]::Pow(2, $Cores) - 1)
$p = Start-Process -FilePath "cmake" -ArgumentList "--build `"$repo\out\build`" --config Release --target $Target -- /m:1 /v:m" `
    -RedirectStandardOutput $log -WindowStyle Hidden -PassThru
Start-Sleep -Milliseconds 300
try { $p.PriorityClass = 'Idle'; $p.ProcessorAffinity = [IntPtr]$mask } catch {}
# Children spawned before the settings applied: enforce on all build tools while running.
while (-not $p.HasExited) {
    Get-Process cl, link, MSBuild, mspdbsrv -ErrorAction SilentlyContinue | ForEach-Object {
        try { if ($_.PriorityClass -ne 'Idle') { $_.PriorityClass = 'Idle'; $_.ProcessorAffinity = [IntPtr]$mask } } catch {}
    }
    Start-Sleep 5
}
"exit $($p.ExitCode)"
Select-String -Path $log -Pattern " error |error C|fatal error" | Select-Object -First 10 | ForEach-Object { $_.Line.Trim() }
