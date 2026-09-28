param([string]$JavaHome = $env:JAVA_HOME)
$ErrorActionPreference = 'Stop'
$renutRoot = Split-Path $PSScriptRoot -Parent
if (-not $JavaHome) { throw 'Informe -JavaHome com o caminho de um JDK 17 ou 21.' }
$env:JAVA_HOME = $JavaHome
$socketDir = Join-Path $renutRoot 'out/java-tmp'
New-Item -ItemType Directory -Force $socketDir | Out-Null
# Windows packaged shells can remap the default Java socket directory.
$previousJavaOptions = $env:JAVA_TOOL_OPTIONS
try {
    $env:JAVA_TOOL_OPTIONS = ($previousJavaOptions + ' "-Djdk.net.unixdomain.tmpdir=' + $socketDir + '"').Trim()
    & (Join-Path $renutRoot 'android/gradlew.bat') -p (Join-Path $renutRoot 'android') assembleDebug --no-daemon --console=plain
    if ($LASTEXITCODE) { throw "Compilacao falhou: $LASTEXITCODE" }
} finally { $env:JAVA_TOOL_OPTIONS = $previousJavaOptions }
