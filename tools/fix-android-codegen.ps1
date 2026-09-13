$ErrorActionPreference = 'Stop'
$renutRoot = Split-Path $PSScriptRoot -Parent
$generated = Join-Path $renutRoot 'generated/android'
# RexGlue 0.10.0 emits a fatal branch despite this label belonging to the same
# emitted function. Replace only this exact, reviewed instruction pair.
$before = 'if (!ctx.cr6.eq) REX_FATAL("Unresolved branch from 0x82C79F68 to 0x82C79EE4");'
$after = 'if (!ctx.cr6.eq) goto loc_82C79EE4;'
$needle = $before
$matches = @(Get-ChildItem -LiteralPath $generated -Filter '*.cpp' | Where-Object { [IO.File]::ReadAllText($_.FullName).Contains($before) })
if ($matches.Count -eq 0) {
    # Incremental codegen may keep the already-corrected translation unit.
    $needle = $after
    $matches = @(Get-ChildItem -LiteralPath $generated -Filter '*.cpp' | Where-Object { [IO.File]::ReadAllText($_.FullName).Contains($after) })
}
if ($matches.Count -ne 1) { throw "Esperado exatamente um desvio conhecido; encontrados $($matches.Count). Regenere antes de aplicar." }
$path = $matches[0].FullName
$source = [IO.File]::ReadAllText($path)
if ([regex]::Matches($source, [regex]::Escape($needle)).Count -ne 1) { throw 'Desvio duplicado inesperado.' }
$start = $source.LastIndexOf('DEFINE_REX_FUNC(', $source.IndexOf($needle))
$body = $source.Substring($start, $source.IndexOf($needle) - $start)
if (-not $body.StartsWith('DEFINE_REX_FUNC(sub_82C79EE4)') -or -not $body.Contains('loc_82C79EE4:')) {
    throw 'O destino não pertence à função esperada; correção não aplicada.'
}
if ($needle -eq $before) { [IO.File]::WriteAllText($path, $source.Replace($before, $after)) }
$unresolved = @(Get-ChildItem -LiteralPath $generated -Filter '*.cpp' | Where-Object { [IO.File]::ReadAllText($_.FullName).Contains('REX_FATAL("Unresolved') })
if ($unresolved.Count) { throw 'Há outros desvios não resolvidos; revise antes de compilar.' }
Write-Host 'Desvio interno 0x82C79F68 corrigido e destinos não resolvidos verificados.'
