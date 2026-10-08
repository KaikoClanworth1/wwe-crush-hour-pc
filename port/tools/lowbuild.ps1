# Builds the runner gently: Idle priority, limited to a few cores (child cl.exe processes inherit both).
param([int]$Cores = 6, [string]$Target = "ps2EntryRunner")
$root = "D:\Xbox Games Ports\WWE Crush Hour"
$log = "$root\recomp\build_log.txt"
$mask = [int64]([math]::Pow(2, $Cores) - 1)
$p = Start-Process -FilePath "cmake" -ArgumentList "--build `"$root\PS2Recomp\out\build`" --config Release --target $Target -- /m:1 /v:m" `
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
