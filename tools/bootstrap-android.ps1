$ErrorActionPreference = 'Stop'
$renutRoot = Split-Path $PSScriptRoot -Parent
$deps = @(
    @('plume', 'https://github.com/renderbag/plume.git', 'd890ac899e505fb30040e037a4037cdeca68f033'),
    @('libadrenotools', 'https://github.com/bylaws/libadrenotools.git', '8fae8ce254dfc1344527e05301e43f37dea2df80')
)
foreach ($dep in $deps) {
    $path = Join-Path $renutRoot ('external/' + $dep[0])
    if (-not (Test-Path -LiteralPath $path)) {
        git clone --no-checkout $dep[1] $path
        if ($LASTEXITCODE) { throw 'Falha ao baixar dependencia.' }
        git -C $path checkout --detach $dep[2]
        if ($LASTEXITCODE) { throw 'Falha ao fixar revisao.' }
    }
    $actual = git -C $path rev-parse HEAD
    if ($actual -ne $dep[2]) { throw "Revisao diferente em $path; preserve suas alteracoes e revise manualmente." }
    git -C $path submodule update --init --recursive
    if ($LASTEXITCODE) { throw 'Falha nos submodulos.' }
}
# Tesselacao no Plume (topologia de patches, estagios hull/domain), que o
# renderizador precisa para a agua do jogo. Aplicado uma vez, como o patch do
# SDK: se ja estiver aplicado, o check reverso passa e nada e reescrito.
$plume = Join-Path $renutRoot 'external/plume'
$plumePatch = Join-Path $renutRoot 'android/rexglue/plume-tessellation.patch'
git -C $plume apply --reverse --check $plumePatch 2>$null
if ($LASTEXITCODE) {
    git -C $plume apply --check $plumePatch
    if ($LASTEXITCODE) { throw 'Patch de tesselacao incompativel com alteracoes locais; nenhum arquivo sobrescrito.' }
    git -C $plume apply $plumePatch
    if ($LASTEXITCODE) { throw 'Falha ao aplicar a tesselacao no Plume.' }
}
