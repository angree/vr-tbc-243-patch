@echo off
powershell -NoProfile -ExecutionPolicy Bypass -Command "$d='%~dp0';iex((gc -LiteralPath '%~f0'|select -Skip 4)-join[char]10)"
pause
exit /b
$ErrorActionPreference='Stop'
# ---- edit these two if you want a different mode ----
$RES     = '1024x768'
$REFRESH = '75'
# ----------------------------------------------------
Write-Host ''
Write-Host '  WoW 2.4.3 VR patch - VR mode setup' -ForegroundColor Cyan
Write-Host ('  {0}, fullscreen, {1}Hz, multicore' -f $RES,$REFRESH) -ForegroundColor Cyan
Write-Host ''

$cfg = Join-Path $d 'WTF\Config.wtf'

# refuse to run while THIS install's WoW is open (the game overwrites Config.wtf on exit)
$running = Get-CimInstance Win32_Process -Filter "Name='WoW.exe'" -ErrorAction SilentlyContinue |
           Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($d,[System.StringComparison]::OrdinalIgnoreCase) }
if ($running) { Write-Host '  ERROR: WoW is running from this folder. Close it first, then re-run.' -ForegroundColor Red; return }

if (-not (Test-Path $cfg)) { Write-Host "  ERROR: $cfg not found. Launch WoW once (to create it), then re-run." -ForegroundColor Red; return }

# back up first
Copy-Item $cfg "$cfg.bak" -Force
Write-Host "  Backup saved: WTF\Config.wtf.bak"
Write-Host ''

# use ALL of the machine's cores (mask = 2^cores - 1), capped at 32 for a sane value
$cores = [Math]::Min([Environment]::ProcessorCount, 32)
$mask  = ([long]1 -shl $cores) - 1

$set = [ordered]@{
  'gxResolution'        = $RES
  'gxWindow'            = '0'        # 0 = fullscreen
  'gxRefresh'           = $REFRESH
  'maxfps'              = $REFRESH
  'M2UseThreads'        = '1'        # model worker threads (multicore)
  'componentThread'     = '1'        # texture worker thread (multicore)
  'processAffinityMask' = "$mask"    # let the game use every core on THIS machine
}

$lines = Get-Content $cfg
foreach ($k in $set.Keys) {
  $v   = $set[$k]
  $esc = [regex]::Escape($k)
  if ($lines -match ('^SET {0} "' -f $esc)) {
    $lines = $lines -replace ('^SET {0} ".*"' -f $esc), ('SET {0} "{1}"' -f $k,$v)
  } else {
    $lines += ('SET {0} "{1}"' -f $k,$v)
  }
  Write-Host ('  {0,-20} = {1}' -f $k,$v)
}

Set-Content $cfg $lines -Encoding ascii
Write-Host ''
Write-Host ('  DONE. {0} cores detected. Launch WoW.' -f [Environment]::ProcessorCount) -ForegroundColor Green
Write-Host '  (To undo: rename Config.wtf.bak back to Config.wtf)' -ForegroundColor DarkGray
