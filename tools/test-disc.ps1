param([string]$JavaHome = $env:JAVA_HOME, [string]$IsoPath)
$ErrorActionPreference = 'Stop'
if (-not $JavaHome) { throw 'Informe um JDK 17 ou superior em -JavaHome.' }
$renutRoot = Split-Path $PSScriptRoot -Parent
Push-Location $renutRoot
try {
    New-Item -ItemType Directory -Force out/java-tests | Out-Null
    & (Join-Path $JavaHome 'bin/javac.exe') -encoding UTF-8 -d out/java-tests `
        android/app/src/main/java/org/renut/android/DiscImage.java `
        android/app/src/main/java/org/renut/android/GameFiles.java tools/DiscImageTest.java
    if ($LASTEXITCODE) { throw 'Falha ao compilar os testes.' }
    $testArguments = @('-cp', 'out/java-tests', 'DiscImageTest')
    if ($IsoPath) { $testArguments += $IsoPath }
    & (Join-Path $JavaHome 'bin/java.exe') @testArguments
    if ($LASTEXITCODE) { throw 'Teste do leitor de disco falhou.' }
} finally { Pop-Location }
