# Renderizador Plume nativo — estado em 18/09/2026

## O que existe

- **Seleção de renderizador.** Tela inicial → "Alternar renderizador". Preferência
  `renderer` (`xenos` ou `plume`), passada à `RuntimeActivity` e ao nativo, que grava a
  cvar `renut_renderer`. Padrão: `xenos` (RexGlue, o caminho jogável).
- **Camada D3D** (`android/rexglue/d3d_layer.*`). Sobrescreve 218 funções D3D do jogo
  (lista gerada em `d3d_functions.inc` a partir do mapa de símbolos do Banjo de
  etonedemid/reNut_linuxfixes, `config/renut_funcs2.toml`). No modo `xenos` só conta e
  repassa ao original. No modo `plume`, handlers registrados podem assumir a chamada.
  Relatório `Renut: D3D calls` no log a cada 600 swaps.
- **Captura de shaders.** Todo contêiner passado a `CreateVertexShader`/
  `CreatePixelShader` é salvo em `files/shader-dump/{vs,ps}_<hash>.bin`. Primeira sessão
  até o menu: 238 shaders.
- **Modo `plume`** (etapa 1). O Xenos emulado continua ativo **sem apresentação** (sem
  app context não cria presenter), mantendo fences, vblank e swaps. `PlumeRenderer`
  (`plume_renderer.*`) assume a janela com o mesmo carregador Vulkan da sessão (Turnip
  funciona) e apresenta, em cada `D3DDevice_Swap`, a cor do último `Clear` do jogo.
  Verificado no SM-S918B: `Plume renderer started on Turnip Adreno (TM) 740`.

## Pipeline offline de shaders (WSL)

XenosRecomp do UnleashedRecomp compilado em `~/xr` no WSL (Ubuntu 26.04; toolchain via
`wsl -u root apt-get`; o `zstd` do repositório precisou ser reclonado, v1.5.6). Nos 105
contêineres embutidos no executável do Banjo:

| Resultado | Quantidade | Causa |
| --- | --- | --- |
| Convertidos | 41 | — |
| Falha | 51 | `vfetch` em endereço fora da declaração de vértices (busca manual) |
| Falha | 13 | export para `eA` (32): memexport, partículas |

Ambos exigem estender o XenosRecomp. Os 238 shaders capturados em jogo ainda não foram
passados pela ferramenta.

## Achado que muda o plano

O modelo do UnleashedRecomp intercepta `D3DDevice_Draw*`. **O Banjo não chama essas
funções**: em 600 quadros (~26 desenhos por quadro medidos no processador de comandos)
nenhuma função D3D interceptada é chamada nessa frequência; as mais chamadas são
helpers de ring (`BeginRingAlloc` ~5/quadro, `StartNewSegment`, `BlockOnFence`). Os
únicos pontos que montam `DRAW_INDX_2` no código são internos do D3D (`Resolve`, codec
de vídeo). Os desenhos do jogo chegam por command buffers pré-gravados ou por código
embutido pelo LTCG.

Caminhos possíveis, a decidir:
1. **Nível do motor** (como o Skate 3 Mobile): localizar as funções de submissão de malha
   do motor da Rare e reconstruir a cena no Plume.
2. **Nível PM4 enxuto**: o Plume consome o mesmo stream de comandos que o Xenos, mas com
   render targets reais no lugar da EDRAM emulada e shaders pré-compilados.
3. Localizar a gravação/execução de command buffers (`BeginCommandBuffer`,
   `RunCommandBuffer`) e interceptar ali.

Próximo passo imediato: imprimir todos os contadores (não só os 40 maiores) e medir
`RunCommandBuffer`, `BeginCommandBuffer`/`EndCommandBuffer` e os `SetPending_*` durante o
jogo, para escolher entre 1, 2 e 3.
