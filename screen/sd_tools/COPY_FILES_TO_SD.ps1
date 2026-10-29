# RESET_SD_v1.ps1 - wipe the old DWIN_SET on the SD card, copy the v1 files, verify every byte
param([string]$Drive = "F:")
$ErrorActionPreference = "Stop"
$src = Join-Path $PSScriptRoot "..\SD_CARD\DWIN_SET"
$dst = "$Drive\DWIN_SET"
$expected = @{
  "120.lib" = "4A3F964BD87333A7A403F6AF5D79EF53"
  "13TouchFile.bin" = "AB2A0D28DE6B77FFDD6C72AFEAD099AB"
  "14ShowFile.bin" = "9CE8D2FD02DBF0CEE2C4CAEEC55663A9"
  "22_Config.bin" = "C947A41EB14844B2A72A8C96D0574685"
  "24_ASC.HZK" = "69EA77BF4BC4317DC0789E1806ADA9EC"
  "28_AR.HZK" = "FDA40EB2167EEF056E6238EE858E8068"
  "32.icl" = "916AE284C1A1DD0FA2E467908840E533"
  "T5LCFG.CFG" = "D2B62C7DF1B6434688DA50A73227935E"
}
function Line($t,$c="White"){ Write-Host $t -ForegroundColor $c }
Line "=== NeuralNexus SD card reset (v1) ===" Cyan
if (!(Test-Path $src)) { Line "Source folder not found: $src" Red; exit 1 }
if (!(Test-Path "$Drive\")) { Line "Drive $Drive not found - put the SD card in the reader." Red; exit 1 }
$vol = Get-Volume -DriveLetter $Drive.TrimEnd(':') -ErrorAction SilentlyContinue
if ($vol) { Line ("Drive {0}  label='{1}'  {2}  size={3:N1} GB" -f $Drive,$vol.FileSystemLabel,$vol.FileSystem,($vol.Size/1GB)) }
if ($vol -and $vol.Size -gt 40GB) { Line "This drive is bigger than 40 GB - it is probably NOT the SD card. Stopping." Red; exit 1 }
Write-Host ""
Line "This will DELETE everything inside $dst and put the v1 files there." Yellow
$a = Read-Host "Type YES to continue"
if ($a -ne "YES") { Line "Cancelled." Yellow; exit 0 }

# 1) wipe
if (Test-Path $dst) { Get-ChildItem $dst -Force | Remove-Item -Recurse -Force }
else { New-Item -ItemType Directory $dst | Out-Null }
$left = @(Get-ChildItem $dst -Force)
if ($left.Count -ne 0) { Line "Could not delete: $($left.Name -join ', ')" Red; exit 1 }
Line "[1] Old files deleted - DWIN_SET is empty." Green
foreach ($f in "DWIN_REPORT.txt","DWIN_PROTOCOL.txt","FORMAT_LOG.txt") { if (Test-Path "$Drive\$f") { Remove-Item "$Drive\$f" -Force } }

# 2) copy
Copy-Item "$src\*" $dst -Force
Line "[2] New files copied." Green

# 3) verify
$ok = $true; $report = @("NeuralNexus SD check v1 - $(Get-Date)", "")
foreach ($k in ($expected.Keys | Sort-Object)) {
  $p = Join-Path $dst $k
  if (!(Test-Path $p)) { $ok=$false; $report += "MISSING  $k"; Line "  MISSING  $k" Red; continue }
  $h = (Get-FileHash $p -Algorithm MD5).Hash
  if ($h -eq $expected[$k]) { $report += "OK       $k"; Line "  OK       $k" Green }
  else { $ok=$false; $report += "BAD      $k"; Line "  BAD      $k" Red }
}
foreach ($x in Get-ChildItem $dst -Force) { if (!$expected.ContainsKey($x.Name)) { $ok=$false; $report += "EXTRA    $($x.Name)"; Line "  EXTRA    $($x.Name) (should not be there)" Red } }
$report += ""
if ($ok) { $report += "RESULT: ALL GOOD - only the v1 files are on the card." ; Line "[3] RESULT: ALL GOOD - only the v1 files are on the card." Green }
else     { $report += "RESULT: PROBLEM - see above." ; Line "[3] RESULT: PROBLEM - see above." Red }
$report | Set-Content "$Drive\SD_CHECK_v1.txt" -Encoding UTF8
Line "Report saved to $Drive\SD_CHECK_v1.txt"
