$ErrorActionPreference = 'Stop'
$renutRoot = Split-Path $PSScriptRoot -Parent
$xex = Join-Path $renutRoot 'assets/default.xex'
if ((Get-FileHash -LiteralPath $xex -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'd57b15b73490ca8334fb2fc7a53e0b21936a187731fa4f891273f4feaf38d3bc') {
    throw 'XEX diferente da revisão europeia validada.'
}
$generator = Join-Path $renutRoot 'external/rexglue-host-0.10.0/win-amd64/bin/rexglue.exe'
if (-not (Test-Path -LiteralPath $generator)) { throw 'Execute tools/bootstrap-rexglue.ps1 primeiro.' }
New-Item -ItemType Directory -Force (Join-Path $renutRoot 'out') | Out-Null
Push-Location $renutRoot
$desktopHelper = Join-Path $renutRoot 'generated/rexglue.cmake'
$helperExisted = Test-Path -LiteralPath $desktopHelper
$savedHelper = if ($helperExisted) { [IO.File]::ReadAllBytes($desktopHelper) } else { $null }
try {
    & $generator --log-level info codegen renut_android_manifest.toml *> out/android-codegen.log
    if ($LASTEXITCODE) { throw 'Geração falhou; consulte out/android-codegen.log.' }
    # SDK 0.10 also overwrites the root helper regardless of output directory.
    Copy-Item -LiteralPath $desktopHelper -Destination (Join-Path $renutRoot 'generated/android/rexglue.cmake')
    & (Join-Path $PSScriptRoot 'fix-android-codegen.ps1')
} finally {
    if ($helperExisted) { [IO.File]::WriteAllBytes($desktopHelper, $savedHelper) }
    elseif (Test-Path -LiteralPath $desktopHelper) { Remove-Item -LiteralPath $desktopHelper }
    Pop-Location
}
Write-Host 'C++ gerado. Execute tools/build-android.ps1 para produzir o código máquina ARM64 e o APK.'
