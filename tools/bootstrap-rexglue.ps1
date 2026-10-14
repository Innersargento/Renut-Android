$ErrorActionPreference = 'Stop'
$renutRoot = Split-Path $PSScriptRoot -Parent
# RexGlue (thirdparty/rexglue-sdk, tag v0.10.0) e Plume (thirdparty/plume) sao submodulos,
# usados como foram publicados: nada e aplicado por cima deles.
git -C $renutRoot submodule update --init --recursive
if ($LASTEXITCODE) { throw 'Falha ao obter os submodulos.' }
$deps = @(
    @('libucontext','https://github.com/kaniini/libucontext.git','49e671dd52ff6791295d8161ad3b6da7dc5f6f9d')
)
foreach ($dep in $deps) {
    $path = Join-Path $renutRoot ('external/'+$dep[0])
    if (-not (Test-Path -LiteralPath $path)) {
        git clone --no-checkout $dep[1] $path
        if ($LASTEXITCODE) { throw 'Falha no download.' }
        git -C $path checkout --detach $dep[2]
        if ($LASTEXITCODE) { throw 'Falha ao fixar revisão.' }
    }
    if ((git -C $path rev-parse HEAD) -ne $dep[2]) { throw "Revisão inesperada em $path; alterações locais preservadas." }
}
$hostDir = Join-Path $renutRoot 'external/rexglue-host-0.10.0'
New-Item -ItemType Directory -Force $hostDir | Out-Null
$archive = Join-Path $hostDir 'sdk.zip'
if (-not (Test-Path -LiteralPath $archive)) {
    Invoke-WebRequest 'https://github.com/rexglue/rexglue-sdk/releases/download/v0.10.0/rexglue-sdk-0.10.0-win-amd64.zip' -OutFile $archive
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'ad4485da853fbe932e9f5dbb643f9b5fe85dc73775ebbad4df6879fe6df0c11b') {
    throw 'Checksum do gerador diferente do esperado.'
}
if (-not (Test-Path -LiteralPath (Join-Path $hostDir 'win-amd64/bin/rexglue.exe'))) {
    Expand-Archive -LiteralPath $archive -DestinationPath $hostDir
}
Write-Host 'RexGlue 0.10.0 (submodulo) e gerador de codigo preparados.'
