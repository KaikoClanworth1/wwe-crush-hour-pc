# WWE Crush Hour - native PC port installer.
# Copies the game data from YOUR copy of the game (WWE Crush Hour, PlayStation 2, USA, SLUS-20385) into GameFiles:
# an .iso image (mounted with Windows' built-in disc image support) or a disc drive / folder holding the disc files.
# Usage: Install WWE Crush Hour.bat [path to .iso | drive or folder]   (no argument: a file dialog opens)
param([string]$Source = "")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root "GameFiles"

Write-Host ""
Write-Host "  WWE Crush Hour - native PC port  (alpha 0.0.1)"
Write-Host "  ---------------------------------------------"
Write-Host "  This copies the game data from your own disc or disc image."
Write-Host ""

if (-not $Source) {
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = "Select your WWE Crush Hour (PS2, USA) disc image"
    $dlg.Filter = "PS2 disc image (*.iso)|*.iso|All files (*.*)|*.*"
    if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
        $Source = $dlg.FileName
    } else {
        $fb = New-Object System.Windows.Forms.FolderBrowserDialog
        $fb.Description = "No image chosen. Select the game disc drive or a folder with the disc's files instead"
        if ($fb.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { Write-Host "  Cancelled."; exit 1 }
        $Source = $fb.SelectedPath
    }
}
if (-not (Test-Path -LiteralPath $Source)) { Write-Host "  Not found: $Source" -ForegroundColor Red; exit 1 }

$mounted = $null
try {
    $item = Get-Item -LiteralPath $Source
    if (-not $item.PSIsContainer) {
        Write-Host "  Mounting $($item.Name) ..."
        $mounted = Mount-DiskImage -ImagePath $item.FullName -PassThru
        $vol = $mounted | Get-Volume
        if (-not $vol.DriveLetter) { throw "The image mounted without a drive letter." }
        $src = "$($vol.DriveLetter):\"
    } else {
        $src = $item.FullName
    }

    $elf = Join-Path $src "SLUS_203.85"
    if (-not (Test-Path -LiteralPath $elf)) {
        throw "SLUS_203.85 is not on this disc - this port needs WWE Crush Hour for PlayStation 2 (USA, SLUS-20385)."
    }
    if ((Get-Item -LiteralPath $elf).Length -ne 23025524) {
        throw "SLUS_203.85 is a different version of the game (this port is built for the USA release, SLUS-20385)."
    }

    Write-Host "  Copying the game data to $dest (about 1.1 GB) ..."
    New-Item -ItemType Directory -Force $dest | Out-Null
    # /E all folders, keeps files that are only in GameFiles (settings, saves, mods).
    & robocopy $src $dest /E /NFL /NDL /NJH /NJS /NP /R:2 /W:1 | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Copy failed (robocopy code $LASTEXITCODE)." }
    # Disc files are read-only; the game only reads them, but make them easy to update or delete.
    Get-ChildItem -LiteralPath $dest -Recurse -File | Where-Object { $_.IsReadOnly } | ForEach-Object { $_.IsReadOnly = $false }

    # Port files that live in GameFiles: default settings and the Mods folder (never overwrite the user's).
    $tmpl = Join-Path $PSScriptRoot "GameFiles"
    if (Test-Path $tmpl) { & robocopy $tmpl $dest /E /XC /XN /XO /NFL /NDL /NJH /NJS /NP | Out-Null }

    Write-Host ""
    Write-Host "  Done. Start the game with 'Play WWE Crush Hour.bat'." -ForegroundColor Green
}
catch {
    Write-Host ""
    Write-Host "  Install failed: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
finally {
    if ($mounted) { Dismount-DiskImage -ImagePath $mounted.ImagePath | Out-Null }
}
