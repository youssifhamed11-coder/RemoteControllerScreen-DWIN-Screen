# prepare_sd.ps1 - Formats the SD card on F: for DWIN (8GB FAT32, 4096 cluster) and copies DWIN_SET
$ErrorActionPreference = 'Stop'
$Host.UI.RawUI.WindowTitle = 'DWIN SD Card Setup'
$src = Join-Path $PSScriptRoot '..\SD_CARD'
$log = @()
function Say($m, $c = 'White') { Write-Host $m -ForegroundColor $c; $script:log += $m }

try {
    if (-not (Test-Path (Join-Path $src 'DWIN_SET\32.icl'))) { throw "Source folder not found: $src" }

    $part = Get-Partition -DriveLetter F
    $disk = $part | Get-Disk
    $sizeGB = [math]::Round($disk.Size / 1GB, 1)

    Say "=============================================="
    Say "  Card found on F:"
    Say "  Disk number : $($disk.Number)"
    Say "  Name        : $($disk.FriendlyName)"
    Say "  Bus type    : $($disk.BusType)"
    Say "  Size        : $sizeGB GB"
    Say "=============================================="

    # Safety checks: never touch a system/boot disk or a non-removable bus
    if ($disk.IsSystem -or $disk.IsBoot) { throw "F: is on a SYSTEM/BOOT disk. Stopping." }
    $isCard = ($disk.BusType -in @('USB', 'SD', 'MMC')) -or ($disk.FriendlyName -match 'Card Reader|SD|MMC')
    if (-not $isCard) { throw "F: is not a USB/SD card (BusType=$($disk.BusType)). Stopping." }
    if ($disk.Size -gt 300GB) { throw "Disk is larger than 300GB - does not look like an SD card. Stopping." }

    Write-Host ""
    Write-Host "  ALL DATA ON THIS SD CARD WILL BE ERASED." -ForegroundColor Red
    $ans = Read-Host "  Type YES to continue"
    if ($ans -ne 'YES') { Say "Cancelled. Nothing was changed." 'Yellow'; Read-Host "Press Enter to close"; exit }

    $n = $disk.Number
    Say "[1/5] Wiping card..." 'Cyan'
    Clear-Disk -Number $n -RemoveData -RemoveOEM -Confirm:$false
    Initialize-Disk -Number $n -PartitionStyle MBR -ErrorAction SilentlyContinue

    Say "[2/5] Creating 8GB partition..." 'Cyan'
    $p = New-Partition -DiskNumber $n -Size 8GB -DriveLetter F -IsActive

    Say "[3/5] Formatting FAT32, 4096-byte clusters..." 'Cyan'
    Format-Volume -Partition $p -FileSystem FAT32 -AllocationUnitSize 4096 -NewFileSystemLabel 'SD' -Confirm:$false -Force | Out-Null
    Start-Sleep -Seconds 2

    Say "[4/5] Copying DWIN_SET + report..." 'Cyan'
    Copy-Item -Path (Join-Path $src '*') -Destination 'F:\' -Recurse -Force

    Say "[5/5] Verifying..." 'Cyan'
    $vol = Get-Volume -DriveLetter F
    Say "  File system : $($vol.FileSystem)"
    Say "  Cluster size: $($vol.AllocationUnitSize) bytes"
    $ok = $true
    foreach ($f in Get-ChildItem (Join-Path $src 'DWIN_SET') -File) {
        $a = (Get-FileHash $f.FullName -Algorithm MD5).Hash
        $b = (Get-FileHash (Join-Path 'F:\DWIN_SET' $f.Name) -Algorithm MD5).Hash
        $st = if ($a -eq $b) { 'OK' } else { $ok = $false; 'MISMATCH' }
        Say ("  {0,-18} {1}" -f $f.Name, $st)
    }
    if ($vol.FileSystem -ne 'FAT32' -or $vol.AllocationUnitSize -ne 4096) { $ok = $false }

    Write-Host ""
    if ($ok) {
        Say "DONE - card is ready. Eject it and put it in the screen." 'Green'
    } else {
        Say "Something is not right - send a photo of this window to Claude." 'Red'
    }
    $log | Out-File 'F:\FORMAT_LOG.txt' -Encoding utf8
}
catch {
    Say "ERROR: $($_.Exception.Message)" 'Red'
    Say "Nothing else was done. Send a photo of this window to Claude." 'Red'
}
Read-Host "Press Enter to close"
