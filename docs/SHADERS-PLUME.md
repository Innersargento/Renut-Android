# Shaders recompilados para o Plume — 19/09/2026

## Correções verificadas no aparelho: aura e terreno

- **Aura dourada do menu:** os resolves da máscara de 8 bits e da aura RGBA
  adjacente eram confundidos porque ambos usam RGBA no host. A união de faixas
  agora exige o mesmo tamanho, pitch e bytes por pixel no guest, no mesmo quadro.
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

## Bundles: por que não entram

- Os bundles (`Bundle/*/*`) são CAFF `07.08.06.0036`: cabeçalho, chunk1 (tabela de
  seções, nomes, e pedaços `u32 recurso, u32 offset, u32 tamanho, u8 seção, u8 log2
  alinhamento`), chunk2 (fixups: listas crescentes de posições de ponteiros por recurso
  e seção), depois `.data`, `.texturegpu`, `.gpu`, `.gpucached`, `.stream`, nessa ordem.
  Leitor em `tools/shaders/caff.py`; formato base do project-grabbed (x1nixmzeng).
- Os cabeçalhos de shader ficam no `.data` dos recursos `aid_model_*` (dados
  `rendergraph 26.09.07.0039`), seguidos de 40 bytes (vertex) ou 872 bytes (pixel)
  zerados: espaço do objeto D3D montado no carregamento. **O microcódigo não vem depois.**
- Verificação (`tools/shaders/xenos_ucode.py`, `exact_ucode`: control flow bem formado e
  execs cobrindo exatamente o tamanho declarado; aceita 105/105 do XEX): lidos de forma
  contígua, só 8 de ~42 mil cabeçalhos dos bundles têm microcódigo válido.
- Consequência: rodar o XenosRecomp no diretório do jogo "funciona" (4.185 SPIR-V
  válidos), mas ~78% têm literais com o cabeçalho do contêiner seguinte e corpo sem
  sentido. **Não usar essa saída.**
- O microcódigo também não está no pedaço `.gpu` do próprio modelo (varredura exata de
  todos os offsets, e os 17 ponteiros `.data→.gpu` do modelo apontam para vertex/index
  buffers). Falta descobrir de onde o motor tira a parte física.

## Próximo passo: captura em runtime pelo objeto em uso

Os shaders dos modelos parecem ser objetos in-place (`XGSetVertexShaderHeader` +
`XGRegisterVertexShader`), que **não passam por `CreateVertexShader`/`CreatePixelShader`**,
os únicos pontos que `DumpShader` (`android/rexglue/d3d_layer.cpp`) captura hoje — por
isso só 238 capturas até o menu. `D3DDevice_SetVertexShader` (`sub_8222A0A8`) e
`D3DDevice_SetPixelShader` (`sub_82226730`) já são interceptados. Plano:

1. Nesses dois, para cada objeto novo, salvar os bytes ao redor do objeto (cabeçalho +
   reserva) e procurar no objeto o ponteiro físico cujo destino passa em `exact_ucode`.
2. Montar o contêiner completo (virtual + físico) e gravar em `shader-dump`, com o mesmo
   hash que o Plume vai usar na busca.
3. Rodar `recompile_shaders.sh` sobre `xex.bin` + dumps.
4. Opcional: com o layout do objeto conhecido, seguir o fixup correspondente no CAFF e
   extrair tudo offline.

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
