# Água do Nuts & Bolts: os patches tesselados no Plume

## O que o jogo faz

A água é desenhada depois do resolve da cena, numa sequência de quatro desenhos:

```
prim=1  n=1   vs=9CD3AF1135A8FEE3 ps=60E8DEF931549772 tess=2 indexed=2
prim=1  n=99  vs=1DF3417E061967C2 ps=88680E4C8A6BCFFE tess=2 indexed=2
prim=1  n=512 vs=3D0794ADDBF5A4F3 ps=60E8DEF931549772 tess=2 indexed=2
prim=17 n=99  vs=F09B139EE9188255 ps=3277104CE11D8635 tess=2 indexed=0
```

As três listas de pontos são passagens de *memory export*: escrevem na memória
do guest os fatores de tesselação. O quarto desenho é `kTrianglePatch` em modo
adaptativo: 99 índices, três pontos de controle por patch, 33 patches. O
`rexglue` registra o mesmo uso para o título em `primitive_processor.cpp`:
«4D5307E6 — water — adaptive».

Não faltava patch nem dado do jogo: o Plume é que descartava os quatro desenhos,
porque `kTrianglePatch` caía no `default` do `switch` de topologia.

## O que o Xenos define

De `spirv_translator.cpp`, para `kTriangleDomainPatchIndexed`, o domain shader
recebe `r0.xyz` = coordenada do domínio em ordem **zyx** e `r1.x` = índice do
patch. O vertex shader da água confirma: ele mistura três pontos de controle
com `r2 = P0*r0.z + P1*r0.y + P2*r0.x`, que com essa ordem é `P0*u + P1*v +
P2*w`. Os pontos de controle são buscados **pelo índice do patch**, não pelo
índice do vértice.

## Por que a expansão é feita no Plume

O driver em uso no aparelho de teste — Turnip, sobre o Adreno 740 — responde
`tessellationShader = false` em `vkGetPhysicalDeviceFeatures2`, embora exponha
`geometryShader`. Sem estágio de tesselação no driver, não há hull shader a
escrever, e é também por isso que o renderizador Xenos não desenha a água no
mesmo aparelho.

O Plume então expande cada patch ele mesmo, nos mesmos triângulos que o
tesselador faria, e roda o shader do guest uma vez por ponto gerado. O nível é
o teto que o jogo declara em `VGT_HOS_MAX_TESS_LEVEL`, limitado a 16: os
fatores por aresta que o jogo calcula dependem de memory export, que o Plume
ainda não emula.

## O que entrou

### XenosRecomp (`tools/shaders/renut-xenosrecomp.patch`)

Um shader que o jogo usa como estágio de domínio é recompilado uma segunda vez,
sob a hash do contêiner XOR `RENUT_DOMAIN_SHADER_HASH_TAG`. Nada no contêiner
diz qual estágio é — quem decide é o desenho — então a lista vem de
`tools/shaders/domain-shaders.txt`, alimentada pelo registro de desenhos, que
nomeia o vertex shader de todo desenho de patch.

Nessa segunda forma:

- `r0.xyz` e `r1.x` vêm de um `float4` de entrada na location
  `RENUT_DOMAIN_INPUT_LOCATION`, que o Plume preenche: xyz os pesos do ponto no
  patch, w o índice do patch;
- `r0` e `r1` entram marcados como escritos, o que faz cada `vfetch` dos pontos
  de controle sair pelo `vfetchLive` — lendo o stream pelo índice do patch, em
  vez de virar atributo do input assembler.

### Plume (`android/rexglue/plume_gpu.cpp`)

`kTrianglePatch` passa a montar, por patch, uma grade de `nível × nível`
triângulos sobre as coordenadas do triângulo, num stream de `float4` por
vértice, e desenha isso como lista de triângulos com o shader de domínio no
lugar do vertex shader. A tabela de buscas vivas usada é a da forma de domínio.

## Validação

`spirv-val --target-env vulkan1.1` sem falhas nos 1.802 shaders do cache
regerado (1.801 mais a variante de domínio). No SM-S918B com Plume/Turnip, os
dois desenhos `prim=17` da passagem inicial passam de `dropped` a desenhados;
captura em `out/graphics-next/water-drawn.png` e registro em
`out/graphics-next/water-sw-trace.txt`.

## O que falta

- **Memory export**, para os fatores adaptativos reais por aresta em vez do teto
  do registrador. Exige emitir as escritas `eA`/`eM#` no recompilador — hoje vão
  para um registrador descartado — e, no Plume, rodar esses desenhos com o
  rasterizador descartado sobre um buffer que o desenho seguinte leia.
- **Patches de quadriláteros** (`kQuadPatch`), que outros cenários usam para o
  terreno. A expansão e a inicialização de registradores são diferentes.
- O caminho por hardware, se algum driver do aparelho passar a expor
  tesselação; a expansão atual não depende dele.
