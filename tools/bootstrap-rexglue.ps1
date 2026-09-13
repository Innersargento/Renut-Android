$ErrorActionPreference = 'Stop'
$renutRoot = Split-Path $PSScriptRoot -Parent
$deps = @(
    @('rexglue-android','https://github.com/rexglue/rexglue-sdk.git','f5337cdc947ff6d4c4196737e2c807a48f2a1fc2'),
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
$sdk = Join-Path $renutRoot 'external/rexglue-android'
$submodules = @('libmspack','tomlplusplus','simde','xxHash','spdlog','fmt','utfcpp','o1heap','sdl3','imgui','FFmpeg','glslang','vulkan-headers','vulkan-memory-allocator','spirv-tools','spirv-headers') | ForEach-Object {'thirdparty/'+$_}
git -C $sdk submodule update --init --depth 1 -- $submodules
if ($LASTEXITCODE) { throw 'Falha ao obter submódulos.' }
$patch = Join-Path $renutRoot 'android/rexglue/android-platform.patch'
git -C $sdk apply --reverse --check $patch 2>$null
if ($LASTEXITCODE) {
    git -C $sdk apply --check $patch
    if ($LASTEXITCODE) { throw 'Patch incompatível com alterações locais; nenhum arquivo sobrescrito.' }
    git -C $sdk apply $patch
    if ($LASTEXITCODE) { throw 'Falha ao aplicar adaptações Android.' }
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
Write-Host 'RexGlue 0.10.0 e adaptações Android preparados.'
