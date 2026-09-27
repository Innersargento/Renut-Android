param([string]$NdkHome = $env:ANDROID_NDK_HOME)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$video = Join-Path $root 'out/ffmpeg-video'
$source = Join-Path $video 'source'
$script = Join-Path $root 'tools/build-video-ffmpeg.sh'
$library = Join-Path $video 'install/lib/libavformat.a'
# The stamp holds the build script's hash, so a change to its configure flags
# rebuilds from a fresh copy of the source.
$stamp = Join-Path $video 'install/renut-video.stamp'
$expected = (Get-FileHash -LiteralPath $script -Algorithm SHA256).Hash
if ((Test-Path -LiteralPath $library) -and (Test-Path -LiteralPath $stamp) -and
    ((Get-Content -LiteralPath $stamp -Raw).Trim() -eq $expected)) { return }

if (-not $NdkHome) {
    $properties = Join-Path $root 'android/local.properties'
    if (-not (Test-Path -LiteralPath $properties)) { throw 'Defina ANDROID_NDK_HOME ou android/local.properties.' }
    $sdk = (Get-Content -LiteralPath $properties | Where-Object { $_ -match '^sdk\.dir=' } | Select-Object -First 1) -replace '^sdk\.dir=', ''
    $sdk = $sdk -replace '\\:', ':' -replace '\\\\', '\'
    $NdkHome = Join-Path $sdk 'ndk/29.0.14206865'
}
if (-not (Test-Path -LiteralPath $NdkHome)) { throw "NDK não encontrado: $NdkHome" }

if (Test-Path -LiteralPath $video) { Remove-Item -LiteralPath $video -Recurse -Force }
New-Item -ItemType Directory -Force -Path $video, $source | Out-Null
$archive = Join-Path $video 'ffmpeg-source.tar'
$upstream = Join-Path $root 'external/rexglue-android/thirdparty/FFmpeg'
& git -C $upstream archive "--output=$archive" HEAD
if ($LASTEXITCODE) { throw 'Não foi possível copiar o FFmpeg fixado pelo RexGlue.' }
& tar -xf $archive -C $source
if ($LASTEXITCODE) { throw 'Não foi possível extrair o código do FFmpeg.' }
Remove-Item -LiteralPath $archive
# The pinned SDK tracks a custom config.h. FFmpeg's own configure must
# generate one for this separate Android video build, out of tree.
$config = Join-Path $source 'config.h'
if (Test-Path -LiteralPath $config) { Remove-Item -LiteralPath $config }

$bash = 'C:/msys64/usr/bin/bash.exe'
if (-not (Test-Path -LiteralPath $bash)) { throw 'MSYS2 bash não encontrado em C:/msys64/usr/bin/bash.exe.' }
& $bash $script $NdkHome $video
if ($LASTEXITCODE) { throw "Compilação do FFmpeg de vídeo falhou: $LASTEXITCODE" }
if (-not (Test-Path -LiteralPath $library)) { throw 'O FFmpeg de vídeo não gerou libavformat.a.' }
Set-Content -LiteralPath $stamp -Value $expected -Encoding ascii
