# LiteWall A/B option test. Run from the LiteWall folder (next to LiteWall.exe):
#   powershell -ExecutionPolicy Bypass -File .\tools\ab-test.ps1      (or copy the script next to the exe)
# Each run: start LiteWall with --tray --log --exit-after-stats, sample 30 s, read the SUMMARY line.
# Keep the desktop visible (no fullscreen/maximized windows) and the laptop plugged in while it runs.
param([int]$Secs = 30, [string]$Exe = ".\LiteWall.exe")
if (-not (Test-Path $Exe)) { $Exe = Join-Path $PSScriptRoot "..\LiteWall.exe" }
$log = "$env:APPDATA\LiteWall\litewall.log"
$sets = [ordered]@{
  "A_defaults"          = ""
  "B_displaydesync"     = "video-sync=display-desync"
  "C_extraframes0"      = "hwdec-extra-frames=0"
  "D_dr_off"            = "vd-lavc-dr=no"
  "E_swapint0"          = "d3d11-sync-interval=0"
  "F_fps30"             = "override-display-fps=30"
  "G_noflip"            = "d3d11-flip=no"
  "H_gpunext"           = "vo=gpu-next"
  "I_combo"             = "hwdec-extra-frames=0,vd-lavc-dr=no,override-display-fps=30"
  "J_perMonitor"        = "--per-monitor"
}
$results = @()
foreach ($k in $sets.Keys) {
  Get-Process LiteWall -EA 0 | Stop-Process -Force; Start-Sleep 2
  $o = $sets[$k]
  $argl = "--tray --log --exit-after-stats --stat-secs $Secs"
  if ($o -like "--*") { $argl += " $o" } elseif ($o) { $argl += " --mpv-opts `"$o`"" }
  Write-Host "== $k : $argl"
  $p = Start-Process $Exe -ArgumentList $argl -PassThru
  if (-not $p.WaitForExit(($Secs + 25) * 1000)) { $p | Stop-Process -Force }
  Start-Sleep 1
  Copy-Item $log "$env:TEMP\litewall-$k.log" -Force
  $sum = Select-String $log -Pattern "SUMMARY avg_private=([\d.]+)MB avg_cpu_1s=([\d.]+)" | Select-Object -Last 1
  $rej = (Select-String $log -Pattern "REJECTED" | % { $_.Line -replace '^.*REJECTED ', '' }) -join "; "
  $hw  = (Select-String $log -Pattern "prop hwdec-current = (\S+)" | Select-Object -Last 1)
  $results += [pscustomobject]@{
    Set = $k; Options = $o
    PrivateMB = if ($sum) { [double]$sum.Matches[0].Groups[1].Value } else { $null }
    CpuPct    = if ($sum) { [double]$sum.Matches[0].Groups[2].Value } else { $null }
    Hwdec     = if ($hw) { $hw.Matches[0].Groups[1].Value } else { "?" }
    Rejected  = $rej
  }
}
$results | Sort-Object PrivateMB | Format-Table -AutoSize
$results | Export-Csv "$env:USERPROFILE\Desktop\litewall-ab.csv" -NoTypeInformation
Write-Host "Saved $env:USERPROFILE\Desktop\litewall-ab.csv; per-run logs in $env:TEMP\litewall-*.log"
Write-Host "Restart normally afterwards: Start-Process $Exe"
