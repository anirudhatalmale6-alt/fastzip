<#
  bench.ps1 - before/after benchmark in milliseconds.

  Usage (from a normal PowerShell prompt, no admin needed):

      .\scripts\bench.ps1 -Data C:\path\to\folder-you-zip
      .\scripts\bench.ps1 -Data C:\data -Runs 5
      .\scripts\bench.ps1 -Data C:\data -Reference "C:\tools\yourzip.exe -r {out} {in}"

  -Reference lets you time your existing utility in the same harness.
  Use {out} and {in} as placeholders for the archive path and the input path.
#>
param(
  [Parameter(Mandatory=$true)][string]$Data,
  [string]$Exe = "$PSScriptRoot\..\build\fastzip.exe",
  [int]$Runs = 3,
  [int]$Level = 6,
  [string]$Reference = "",
  [string]$WorkDir = $env:TEMP
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path $Exe))  { throw "fastzip.exe not found at $Exe - build it first (see README)" }
if (-not (Test-Path $Data)) { throw "input path not found: $Data" }

$cores = (Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors
Write-Host ""
Write-Host "fastzip benchmark"
Write-Host "  input   : $Data"
Write-Host "  cores   : $cores"
Write-Host "  runs    : $Runs (best of)"
Write-Host "  level   : $Level"
Write-Host ""

function Measure-Run {
  param([scriptblock]$Action, [string]$Out)
  $best = [double]::MaxValue
  $size = 0
  for ($i = 0; $i -lt $Runs; $i++) {
    if (Test-Path $Out) { Remove-Item $Out -Force }
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $Action
    $sw.Stop()
    if ($sw.Elapsed.TotalMilliseconds -lt $best) { $best = $sw.Elapsed.TotalMilliseconds }
    if (Test-Path $Out) { $size = (Get-Item $Out).Length }
  }
  [pscustomobject]@{ Ms = $best; Bytes = $size }
}

$rows = @()
$out  = Join-Path $WorkDir "bench_fastzip.zip"

# --- 1. baseline: single thread, stock zlib, no block splitting -------
$r = Measure-Run -Out $out -Action { & $Exe --quiet --baseline -l $Level $out $Data }
$rows += [pscustomobject]@{ Configuration = "baseline (1 thread, zlib)"; Ms = $r.Ms; Bytes = $r.Bytes }
$baseMs = $r.Ms

# --- 2. thread scaling ------------------------------------------------
foreach ($t in @(1, 2, 4, 8, $cores) | Select-Object -Unique | Where-Object { $_ -le $cores }) {
  $r = Measure-Run -Out $out -Action { & $Exe --quiet -t $t -l $Level $out $Data }
  $rows += [pscustomobject]@{ Configuration = "fastzip, $t thread(s)"; Ms = $r.Ms; Bytes = $r.Bytes }
}

# --- 3. your existing tool, if supplied -------------------------------
if ($Reference -ne "") {
  $refOut = Join-Path $WorkDir "bench_reference.zip"
  $cmd = $Reference.Replace("{out}", $refOut).Replace("{in}", $Data)
  $r = Measure-Run -Out $refOut -Action { cmd /c $cmd | Out-Null }
  $rows += [pscustomobject]@{ Configuration = "your current tool"; Ms = $r.Ms; Bytes = $r.Bytes }
}

Write-Host ("{0,-34} {1,12} {2,14} {3,10}" -f "configuration", "elapsed ms", "archive bytes", "speed-up")
Write-Host ("{0,-34} {1,12} {2,14} {3,10}" -f ("-" * 34), ("-" * 12), ("-" * 14), ("-" * 10))
foreach ($row in $rows) {
  $sp = if ($row.Ms -gt 0) { "{0:N2}x" -f ($baseMs / $row.Ms) } else { "-" }
  Write-Host ("{0,-34} {1,12:N1} {2,14:N0} {3,10}" -f $row.Configuration, $row.Ms, $row.Bytes, $sp)
}
Write-Host ""
Write-Host "Correctness check (re-inflates every entry and compares CRC):"
& $Exe --verify -l $Level $out $Data
Write-Host ""
