# Shaders recompilados para o Plume — 19/09/2026

## Correções verificadas no aparelho: aura e terreno

- **Aura dourada do menu:** os resolves da máscara de 8 bits e da aura RGBA
  adjacente eram confundidos porque ambos usam RGBA no host. A união de faixas
  agora exige o mesmo tamanho, pitch e bytes por pixel no guest. Entre quadros,
  a união também verifica se a memória guest ainda pertence ao mesmo conteúdo.
  Imagens e descritores substituídos são mantidos até a conclusão da GPU.
- **Rosetas nas montanhas:** o VS `239A125831F7EB9D` busca `TEXCOORD0` nas
  instruções 9 e 11, nos offsets de vértice 20 e 24. A deduplicação pelo nome
  fazia o mapa geral usar o UV repetido da pedra. Buscas repetidas de uma
  semântica agora recebem entradas independentes na tabela `vfetchLive`, com
  layout lido do microcódigo em uso. `verify_cache.cpp` verifica esse caso e
  detecta colisões ou excesso de slots de leitura.
- Texturas 2D passam a carregar os níveis de mip reais, incluindo o mip tail
  compactado, usando o layout do SDK. Texturas empilhadas usam o mesmo cálculo
  de níveis e de distância entre camadas.

Validação: 1.801 shaders recompilados, zero falhas de decodificação ou de
`spirv-val --target-env vulkan1.1`; APK debug compilado e instalado no SM-S918B
com Plume/Turnip. Capturas em `out/graphics-20260919/uv-fixed-first.png` e
`final-menu-check.png`: montanhas sem rosetas e aura dourada sem faixas coloridas.
O cache em `generated/shaders/` foi regenerado; para reproduzir, usar o script
abaixo com o XEX e os contêineres capturados em runtime, incluindo os do terreno.

## Sombras distantes: montagem do atlas

O mapa estático `t15` tem 2048×2048 pixels e é montado em 16 resolves de
512×512, ao longo de 16 quadros. Os endereços usam o layout tiled do Xenos:
um deslocamento horizontal de 512 pixels corresponde a `0x8000` bytes neste
formato, não a uma linha linear. Além disso, as fileiras chegam de baixo para
cima, com bases diferentes e alturas restantes de 512, 1024, 1536 e 2048.

`FindResolvedContaining` recupera coluna e linha pelo layout do SDK e permite
continuar a montagem entre quadros. `MergeResolvedTails` preserva as fileiras
inferiores quando chega a nova base, validando formato, pitch, altura, endereço
e conteúdo da memória guest. Texturas absorvidas aguardam a conclusão da GPU
antes de serem liberadas. A limpeza inicial também precisa sobreviver ao
primeiro desenho: recriar o buffer depois de limpá-lo deixava o primeiro bloco
sem profundidade válida.

A leitura da GPU no SM-S918B confirmou 16 atualizações no atlas e nenhuma
região zerada, contra 93,75% de pixels zerados na implementação original.
Evidências locais em `out/graphics-20260919/shadows-clear-trace.txt`,
`shadow-15-clear-2048x2048-f26.raw` e `shadows-complete-flyby.png`.

Ferramenta: [masterspike52/reNut-XenosRecomp](https://github.com/masterspike52/reNut-XenosRecomp)
(commit `8f781f8`, modo `RENUT_RECOMP`) com o patch `tools/shaders/renut-xenosrecomp.patch`.
Compilada no WSL em `~/rxr`. Reprodução: `tools/shaders/recompile_shaders.sh <entrada> <saída>`.

## Resultado

| Origem | Contêineres | Recompilados | Observação |
| --- | --- | --- | --- |
| XEX (`xex.bin` decifrado) | 105 | **105** | antes: 41 com o XenosRecomp original |
| Bundles CAFF do jogo | ~42 mil cabeçalhos | — | o microcódigo não está junto do cabeçalho; ver abaixo |

Os 105: SPIR-V validado por `spirv-val --target-env vulkan1.1` (0 falhas), média de
14 KB por shader (máx. 57 KB), contra ~100 KB do tradutor Xenos atual. Saída em
`generated/shaders/` (`shader_cache.cpp`, `shader_common.h`, `hlsl/`); header do lado do
runtime em `android/rexglue/shader_cache.h`.

## O que o patch muda no fork

1. **`vfetch` manual em qualquer formato.** O fork só aceitava `32_32_32_32_FLOAT` nas
   buscas fora da declaração de vértices; as 51 falhas do XEX eram `8_8_8_8` (cores) e
   `16_16` inteiro (índices lidos como stream, instanciamento manual). Novo
   `vfetchStreamFormat` em `shader_common.h` desempacota os formatos Xenos (8_8_8_8,
   2_10_10_10, 10_11_11, 11_11_10, 16_16, 16_16_16_16, halfs, 32-bit int/float) com
   sinal, normalização, modo de faixa e `exp_adjust`, como o xenia. **Contrato com o
   host:** os streams sobem com troca de bytes por palavra de 32 bits (k_8in32).
2. **Mini fetch** usa constante de fetch e stride do último fetch completo também no
   caminho manual.
3. **Bug do layout exportado:** `isSigned` vinha de `signedRfModeAll`; o certo é
   `formatCompAll` (`signedRfModeAll` só escolhe o mapeamento para [-1, 1]). Sem isso,
   normais/tangentes SNORM seriam declaradas UNORM no input layout do Plume.
4. `main.cpp`: segue symlinks de diretório; `XENOS_RECOMP_ALLOW_FAILURES=1` grava o cache
   sem os shaders que falharam (desligado por padrão).

## Extração offline dos bundles (21/09/2026)

`tools/shaders/extract_caff_shaders.py <Bundle> <saída> [dumps de runtime]` extrai os
contêineres completos direto dos dados do jogo, sem rodar o jogo.

- Os bundles são CAFF `07.08.06.0036` (leitor em `tools/shaders/caff.py`, formato base do
  project-grabbed): cabeçalho, chunk1 (seções, nomes, pedaços), chunk2 (fixups), depois
  `.data`, `.texturegpu`, `.gpu`, `.gpucached`, `.stream`. `Bundle/50/*` (magic
  `438cb47c`) são CAFFs colados de ponta a ponta (14.330 no total); `Bundle/4f` tem 9
  CAFFs soltos e 142 arquivos XCTD.
- **chunk2** abre com duas listas de fixups, dimensionadas pelos pares de contagem do
  cabeçalho (`h[6]/h[7]` e `h[10]/h[11]`): registros `(u32 pedaço origem, u32 pedaço
  destino, u32 n)`, depois os `n` offsets u32 de cada grupo — palavras dentro do pedaço de
  origem que contêm um offset no pedaço de destino. Os CAFFs soltos usam só a primeira
  lista; os empacotados põem os ponteiros para o pool na segunda. O resto do chunk2 são
  registros de 16 bytes ainda não usados.
- A **parte virtual** (cabeçalho) de cada shader fica no `.data` do modelo; a **parte
  física** fica no recurso `pool` do mesmo CAFF: `.gpu` para pixel shaders (flag
  `0x102A1100`), `.gpucached` para vertex shaders (`0x102A1141`). O modelo tem uma tabela
  de fixups para cada um desses dois pedaços, um ponteiro por shader, **na ordem dos
  cabeçalhos**. É esse ponteiro que vira `objeto+24` / `objeto+32` no carregamento.
  Três recursos avulsos `aid_*shader_banjox_microcode_*` guardam o contêiner contíguo.
- Por isso a leitura contígua falhava e o pareamento por tamanho era ambíguo: o vínculo é
  posicional, via fixup.

Resultado: **42.145 de 42.145 cabeçalhos** pareados, todos passando em `exact_ucode`,
**2.906 contêineres únicos** (490 VS, 2.416 PS). Contra os 1.708 dumps locais: 28.626
extrações idênticas byte a byte a uma captura, 4.922 iguais depois de zerar os campos de
`vfetch` que o D3D reescreve no bind. Dos 3.149 dumps do aparelho (`~/rxr/in-full/dump`),
2.647 dos 2.906 estão lá com o mesmo hash FNV-1a; 259 são novos. As diferenças restantes
são cabeçalhos idênticos com microcódigo diferente (variantes legítimas), conferidas à mão.

**XCTD (`Bundle/4f`).** Os 142 arquivos `0FF512ED` são xcompress LZX decodificado por
segmento com `XMemDecompressSegmentTD`, que só existe na biblioteca da Microsoft. O
build Windows do QuickBMS a inclui (o de Linux não); `tools/shaders/decompress_xctd.sh`
roda o `quickbms.exe` pelo interop do WSL e grava CAFFs comuns (142 de 142, 3,0 GB).
Deles saem mais **140.860 de 140.860 cabeçalhos**, 3.997 contêineres únicos.

**Cobertura final.** Dos 3.149 dumps do aparelho, 2.982 saem byte a byte da extração
offline e 17 vêm do XEX. Os 150 restantes não existem em nenhum dado do jogo (busca pelo
microcódigo inteiro em todos os arquivos, inclusive os descomprimidos): 139 não têm nenhum
shader com o mesmo cabeçalho, 11 são variantes de 1 a 75 bytes de shaders extraídos. São
gerados ou alterados em runtime e só entram pelos dumps.

Recompilação de tudo (XEX, 3.149 dumps, internos do D3D, 259 novos do `Bundle/50` e
1.385 novos do XCTD): **4.887 entradas, 0 falhas de decodificação, 0 falhas no
`spirv-val --target-env vulkan1.1`**, `shader_ucode_map.inc` com 4.886 contêineres (antes
3.242).

No aparelho (SM-S918B, 21/09, quadros 6.901 a 10.501): todos os 35 relatórios com
`no_vs=0 no_ps=0 missing_ucode=0`, até 1.623 shaders em uso, `files/plume-miss` vazio.

Reprodução:

```bash
tools/shaders/decompress_xctd.sh "<jogo>/Bundle/4f" ~/xctd/out
python3 tools/shaders/extract_caff_shaders.py "<jogo>/Bundle" <saída>
python3 tools/shaders/extract_caff_shaders.py ~/xctd/out <saída-xctd>
# entrada: xex.bin + dumps de runtime + internal/ + as duas saídas
tools/shaders/recompile_shaders.sh <entrada> <saída-spirv>
```

## Shaders internos do Direct3D (19/09/2026)

O `Clear` do D3D e os draws de pontos em torno de fences usam shaders próprios do D3D
que estão no XEX como microcódigo cru, sem contêiner, e por isso nunca entravam no
cache (VS `1E6883FCCDE1F688`, `B6C9863F710683EC`, `DDED7E538422AE73`,
`BF9066893FA79ADD`, `AB1E86137A0240E8`, microcódigo). O renderizador grava o microcódigo
de cada VS ausente em `files/plume-miss`; `tools/shaders/wrap_raw_ucode.py` embrulha
cada um num contêiner sem elementos de vértice (todo `vfetch` vira busca manual) e com um
interpolador por registrador exportado (COLOR0 primeiro). Os `.bin` gerados vão no
diretório de entrada do `recompile_shaders.sh`, junto de `xex.bin` e dos dumps.

O Clear busca pela constante de fetch 0, que ganhou a entrada 16 da tabela de streams
(`RENUT_DIRECT_FETCH_STREAM`). O host não desenha o Clear: ele usa uma visão da EDRAM
(meio pitch a 64 bpp, os quatro alvos de cor, 4096 linhas) que só coincide com os alvos
reais no hardware. `IssueD3DClear` lê a cor e a profundidade do primeiro vértice e limpa
os alvos reais de mesma base, formato e pitch (ou o dobro do pitch). Sem ele, o depth que
o passe de DOF restaura no fim do quadro continuava lá quando a primeira faixa do
predicated tiling do quadro seguinte desenhava, e a metade de cima da tela saía preta.

## Partículas: point sprites e memory export (22/09/2026)

As partículas do jogo (borboletas, folhas, faíscas, o brilho ao pegar uma nota) têm duas
partes, e o Plume não fazia nenhuma das duas:

1. **Point sprites.** São listas de pontos (`prim=1`) que o Xenos transforma em quadrados
   alinhados à tela. O Plume descartava todas em `skipped_prim`. Além disso, o VS calculava
   o tamanho do ponto (`oPointSizeEdgeKill.x`) numa variável local, e o PS recebia a
   coordenada dentro do sprite (`zw` do registrador param-gen) como zero fixo.
2. **Memory export.** A simulação roda na GPU. `4CCF520F7AFFA26A` cria as partículas da
   coleta. `26AC9C907CBAEDD4`, `D075408508A3BC6E` e `D38D10B70F954F86` simulam as
   partículas a cada quadro. Todos gravam o estado na memória por `eA`/`eM0`–`eM4`, e os
   desenhos seguintes (`9BC8…`, `ABD6…`, `8070…`) leem esse estado como stream de
   vértices. O recompilador jogava as gravações numa variável `oMemExport` descartada.

**Recompilador** (`renut-xenosrecomp.patch`):
- Todo VS declara por último `oPointSprite` (localização 23,
  `RENUT_POINT_SPRITE_LOCATION`), com o raio em NDC calculado por `renutPointRadius`. O
  tamanho exportado é limitado por `PA_SU_POINT_MINMAX`; `PA_SU_POINT_SIZE` vale quando o
  VS não exporta nenhum (o tamanho começa em -1).
- Todo PS lê `iPointSprite` e monta o param-gen como o hardware.
- `eA` e `eM0`–`eM4` são registradores separados. `renutMemExport` grava o que foi escrito
  quando `eA` é reescrito e no fim do shader. Ela empacota no formato do stream e grava num
  buffer da GPU: `RENUT_SHARED_EXPORT_OFFSET`, com até 8 faixas `(base, tamanho, endereço)`.
- O shader que exporta ganha `RENUT_SHADER_FLAG_MEMEXPORT` no `specConstantsMask`, com a
  primeira e a última constante que alimentam o `eA` nos bits 8–23. O host só procura
  streams nessas constantes: constantes velhas de outros desenhos criavam faixas falsas,
  que engoliam buffers dinâmicos do CPU (triângulos esticados).

**Plume** (`plume_gpu.cpp`, `shaders/point_list.geom`):
- As listas de pontos viram `POINT_LIST`, com um GS que emite quatro vértices por ponto.
  O GS descarta pontos de tamanho zero (pontos usados só para exportar) e pontos com o
  centro fora dos planos próximo e distante.
- `rect_list.geom` repassa a localização 23.
- Cada stream exportado vira uma `ExportRange` persistente: um buffer da GPU que substitui
  a memória do guest para os desenhos seguintes. Barreiras separam gravação e leitura.
- A faixa é comparada uma vez por quadro com a memória do guest, e o que o CPU escreveu
  entra por cima. O que a GPU exportou volta ao guest depois do quadro (writeback), senão
  um reset feito pelo CPU com os mesmos bytes passaria despercebido.
- As faixas ficam sempre em `k8in32`, a forma de qualquer stream de vértices no host.

**As folhas gigantes (resolvido em 25/09/2026).** Eram três defeitos empilhados, todos
visíveis no dump das faixas (`files/exportdump`, gravado nos quadros com trace):

1. **Endian da exportação.** `D38D…` grava o estado das folhas em `16_16_16_16_FLOAT`
   com `k8in16` (c193), e os desenhos leem em `k8in32`. `renutExportEndian` aplica o
   endian da exportação e depois o `k8in32` do host, como o hardware.
2. **Textura 3D sem ligação.** `D38D…` tira o empurrão do vento de uma textura 3D
   (`tfetch3D`, 512×512×1 DXT1 em `19ACF000`). O Plume só ligava texturas 2D, empilhadas
   e cube, então o slot lia o fallback branco. O ruído `×2−1` dava (1,1,1) para todas as
   folhas, e todas andavam na mesma diagonal: no dump, `y−x` e `z−x` eram iguais em
   todo o pool. `GetGuestVolumeTexture` (`plume_gpu.cpp`) e `DecodeTexture3D`
   (`plume_textures.cpp`) decodificam o nível base de texturas `k3D`, tiled ou linear.
3. **Swizzle de 16 bits.** O Direct3D reescreve os `vfetch` no bind, e um elemento de 16
   bits ganha o swizzle `.yxwz`. Um shader que o cache encontra pela chave "loose" foi
   recompilado a partir do microcódigo sem essa reescrita: os contêineres tirados dos
   bundles, como `D38D…`, `ABD6…` e `8070…`. Esses shaders liam X↔Y e Z↔W trocados.
   - O eixo de giro das folhas (`+32`) saía com norma 1,78 em vez de 1,00, e o quaternion
     (`+16`) crescia a cada quadro.
   - O `8070…` gira os cantos do quad por esse quaternion sem normalizar, então a folha
     escalava por |q|²: no dump havia |q|² ≈ 35 e componentes de ±24.000.
   - Correção: o recompilador grava em `ShaderInputElement::dstSwizzle` o swizzle com que
     compilou cada busca. `CompareFetchSwizzle` compara esse swizzle com o da busca viva.
     Quando a diferença é exatamente a troca de metades, o host liga `swapFloats` (input
     assembler) ou `RENUT_LIVE_FETCH_SWAP_HALVES` (`vfetchLive`). Outras divergências,
     como `zyxw` em cores `8_8_8_8`, vão para o log.

Com a leitura corrigida, a exportação grava exatamente como o hardware: X nos bits baixos
(como `PackHalf2x16` no rexglue) e depois o endian. A troca prévia de metades
(`halvesSwapped`), tentada antes, só compensava o defeito 3 para quem lia sem o
swizzle, e errava para os shaders que o têm.
