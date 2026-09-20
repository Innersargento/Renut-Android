# Água do Nuts & Bolts: tesselação no Plume — plano e estado

## O que o jogo faz

A água é desenhada depois do resolve da cena, numa sequência de quatro desenhos
que o Plume hoje descarta inteira:

```
prim=1  n=1   vs=9CD3AF1135A8FEE3 ps=60E8DEF931549772 tess=2 indexed=2
prim=1  n=99  vs=1DF3417E061967C2 ps=88680E4C8A6BCFFE tess=2 indexed=2
prim=1  n=512 vs=3D0794ADDBF5A4F3 ps=60E8DEF931549772 tess=2 indexed=2
prim=17 n=99  vs=F09B139EE9188255 ps=3277104CE11D8635 tess=2 indexed=0
```

As três listas de pontos são passagens de *memory export*: escrevem na memória do
guest os fatores de tesselação. O quarto desenho é `kTrianglePatch` em modo
adaptativo: 99 índices, 3 pontos de controle por patch, 33 patches.

O `rexglue` registra esse mesmo uso para o título: «4D5307E6 — water — adaptive»
em `graphics/primitive_processor.cpp`.

## O que o Xenos define

De `graphics/pipeline/shader/spirv_translator.cpp`, para
`kTriangleDomainPatchIndexed`, o domain shader recebe:

- `r0.xyz` = coordenada do domínio em ordem **zyx**;
- `r1.x` = índice do patch, `r1.y` = 0.

O vertex shader da água confirma: ele mistura três pontos de controle com
`r2 = P0*r0.z + P1*r0.y + P2*r0.x`, que com a ordem acima é
`P0*u + P1*v + P2*w`.

Do hull shader do backend Vulkan do rexglue, em modo adaptativo com domínio
triangular:

```
gl_TessLevelOuter[0..2] = valor[1], valor[2], valor[0]
gl_TessLevelInner[0]    = min dos três
patch_index             = gl_PrimitiveID + base, limitado ao intervalo de índices
```

## Etapas

### 1. Tesselação no Plume — feito

`android/rexglue/plume-tessellation.patch`, aplicado por
`tools/bootstrap-android.ps1`: topologia `PATCH_LIST`, estágios hull e domain,
`patchControlPoints` e a capacidade `tessellationShader` no backend Vulkan.

### 2. Domain shaders no XenosRecomp — pendente

O recompilador não conhece domain shaders: ele inicializa `r0` com o índice do
vértice e transforma todo `vfetch` indexado pelo id do vértice em atributo do
input assembler. Num domain shader os pontos de controle são buscados pelo
índice do patch, e portanto têm de passar pelo caminho `vfetchLive`, que o fork
já tem para buscas indexadas por registrador calculado.

Mudanças no fork (`tools/shaders/renut-xenosrecomp.patch`):

- `dxc_compiler`: alvo `ds_6_0`, mantendo `-fvk-invert-y` (o domain shader é
  quem escreve `SV_Position`).
- `shader_recompiler`: um modo domain que
  - emite `[shader("domain")] [domain("tri")]` e a assinatura correspondente,
  - inicializa `r0 = float4(dom.z, dom.y, dom.x, 0)` e
    `r1 = float4(float(patchId), 0, 0, 0)`,
  - marca `r0` e `r1` como escritos, o que faz os `vfetch` dos pontos de
    controle caírem no `vfetchLive`,
  - não declara os elementos de vértice, mas mantém o mapa de endereços para
    que o `vfetchLive` saiba a semântica de cada busca.
- `main.cpp`: uma lista de hashes a recompilar também como domain shader, com a
  variante gravada numa segunda entrada de cache sob `hash ^ tag`. O
  recompilador não tem como deduzir o estágio do contêiner; a lista é obtida do
  registro de desenhos, que já nomeia o vertex shader de todo desenho
  descartado por primitiva não suportada.

Depois disso, `generated/shaders/` precisa ser regerado com
`tools/shaders/recompile_shaders.sh`.

### 3. Hull e vertex shader próprios — pendente

Em `android/rexglue/shaders/`, no mesmo formato do `rect_list.geom` já existente
(`tools/shaders/build_host_shaders.sh` os compila para arrays C):

- `patch.vert`: repassa o fator de tesselação lido do stream, um por ponto de
  controle.
- `patch_tri.tesc`: `layout(vertices = 1) out`, três entradas, escreve os níveis
  como o Xenos define acima.

O índice do patch não precisa atravessar a interface: o domain shader o lê de
`SV_PrimitiveID`.

### 4. O desenho no Plume — pendente

`kTrianglePatch` deixa de cair no `default` da topologia: pipeline com
`PATCH_LIST`, `patchControlPoints = 3`, o vertex e o hull próprios, o domain
shader recompilado do guest e o pixel shader do guest.

Com fator fixo (o máximo do intervalo dos registradores) a água já aparece, sem
depender do memory export. É o primeiro ponto em que dá para comparar com o
Xenos.

### 5. Memory export — pendente

Os fatores adaptativos reais vêm das listas de pontos. Exige emitir as escritas
`eA`/`eM#` no recompilador — hoje elas vão para um registrador descartado — e,
no Plume, executar esses desenhos com o rasterizador descartado escrevendo num
buffer que o desenho seguinte lê como stream, em vez de recopiar a memória do
guest a cada quadro.

## Por que o Xenos também não desenha a água

O backend Vulkan do rexglue tem tesselação e memory export, então o defeito ali é
outro e ainda não foi investigado. O que já está demonstrado é que não falta
patch nem dado do jogo: os quatro desenhos chegam ao renderizador.
