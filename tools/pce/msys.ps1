# Runs a tools/pce/*.sh script in the MSYS2 UCRT64 shell from PowerShell, so the output can be watched.
# Usage: & .\tools\pce\msys.ps1 build_run   (or recompile_build, build, run, package)
# MSYS2 is looked up in $env:MSYS2_ROOT, then C:\msys64, D:\msys64, D:\Apps\msys64.
param([Parameter(Mandatory = $true)][string]$Script)
$roots = @($env:MSYS2_ROOT, 'C:\msys64', 'D:\msys64', 'D:\Apps\msys64') | Where-Object { $_ }
$bash = $roots | ForEach-Object { Join-Path $_ 'usr\bin\bash.exe' } | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $bash) { Write-Error 'MSYS2 not found: set MSYS2_ROOT to its folder (e.g. C:\msys64)'; exit 1 }
$env:MSYSTEM = 'UCRT64'; $env:CHERE_INVOKING = '1'
$sh = (Join-Path $PSScriptRoot "$Script.sh") -replace '\\', '/'
& $bash -l $sh
