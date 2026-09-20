# Plume — desempenho na Showdown Town, 20/09/2026

Aparelho: Samsung SM-S918B, Adreno 740, driver Turnip. Renderizador Plume,
`plume-skip=0`. Medições no mesmo trecho da fase, com a bateria acima de 50% —
abaixo disso o gerenciamento do aparelho limita a GPU a 348 MHz dos 719 MHz
disponíveis, e qualquer número colhido assim não vale.

## Como o quadro se divide

O relatório de quadro do Plume passou a registrar tempo de parede
(`frame_ms`, `pipeline_ms`, `submit_ms`), o número de passes de render e a
contagem de swaps do próprio jogo (`guest_swaps`). Essa última resolveu uma
ambiguidade: o Plume apresenta exatamente uma vez por `VdSwap` do jogo, 1:1, de
modo que `frame_ms` é o tempo de quadro que o jogador vê.

Ponto de partida, no trecho mais pesado:

```
249 ms/quadro = 121 ms gravando a lista de comandos
              +  90 ms esperando a GPU
              +  38 ms compilando pipelines
```

Ao encerrar a sessão, no mesmo trecho: **~150 ms por quadro**, com ~60 ms de
lista de comandos, ~85 ms de GPU e ~2 ms de pipelines.

## O que rendeu

- **Cache de pipelines do Vulkan, persistido em disco.** O Plume criava todo
  pipeline com `VK_NULL_HANDLE` como cache, então o Turnip recompilava tudo a
  cada execução, na thread que grava a lista, com a GPU parada. `pipeline_ms`
  caiu de 63.954 ms para 6 ms por 300 quadros. Não levanta o FPS por si —
  levanta o tempo de carregamento, e a GPU passou de 9-24% para 83% ocupada.
- **Layout de textura memoizado por constante de busca.** A conta era refeita em
  cada slot de cada desenho: 169 mil vezes por quadro para algumas centenas de
  texturas. Cortou a lista de comandos de 121 ms para 64 ms por quadro. Foi o
  maior ganho da sessão.
- **Teto de altura dos alvos.** O Clear do Direct3D opera sobre a visão da EDRAM,
  de 4096 linhas, e essa altura inflava permanentemente os alvos reais. Um alvo
  1280 de largura caiu de 21 MB para 5,2 MB. O ganho de tempo medido depois se
  mostrou indistinguível de ruído; o ganho de memória é real.
- **Só os slots de textura que o shader amostra.** Lido das instruções `tfetch`.
  Valeu 4% da lista de comandos, não o ganho grande que o perfil sugeria: o laço
  já saía cedo nos slots sem textura.

## O que não rendeu, e por quê importa

- **Tirar a contagem de chamadas D3D do caminho quente.** O jogo consulta
  `D3D_CBlocker_Check` 8,09 bilhões de vezes por 600 swaps enquanto espera uma
  fence, e o interceptador abria com um incremento atômico: 24,5% dos ciclos do
  processo num só `__aarch64_ldadd8_relax`. Removido, o FPS não mudou nada — os
  ciclos liberados viraram mais espera. Custava calor e bateria, não tempo.
- **Estreitar a `renderArea` do render pass.** O Plume abre todo passe sobre o
  anexo inteiro, e a cena desenha uma faixa de 1280x384 num alvo bem mais alto.
  Medido: nenhuma diferença. Revertido por isso — a mudança exigia um seam no
  CMake e estado por passe, e complexidade sem ganho medido é dívida.

## O que o quadro é, hoje

O tempo de GPU é **proporcional ao número de desenhos**, a 12-17 µs cada, e não
a pixels nem a tiles. Um Adreno 740 deveria executar um desenho em bem menos de
1 µs, então estamos uma ordem de grandeza fora e a causa não está no tráfego de
tile — foi o que a `renderArea` e o teto de altura descartaram.

Composição de um quadro traçado (3.873 desenhos):

| origem | desenhos | |
| --- | --- | --- |
| Cena, em duas faixas de predicated tiling | 2.286 | 59% |
| Mapa de sombras 768x768 | 885 | 23% |
| Alvo 1024x1024 | 434 | 11% |
| Passe reduzido 320x180 | 222 | 6% |

As duas faixas da cena são `scissor (0,0)-(1280,384)` com `window (0.5, 0.5)` e
`scissor (0,0)-(1280,336)` com `window (0.5, -383.5)`: 384 + 336 = 720. Elas
**não** são a mesma cena repetida — o jogo já descarta por faixa, 1.519 contra
767 desenhos —, então unificá-las economiza a geometria que cruza a divisa mais
um ciclo de limpar/resolver, não metade do trabalho.

## Próximo passo

Candidata número um, ainda não testada: os shaders leem constantes e índices de
descritor por `vk::RawBufferLoad` sobre endereços vindos de push constants, e
não por uniform buffer. Num Adreno, um UBO é promovido para memória de
constantes rápida; uma carga crua por ponteiro não é.

**Medir antes de mexer.** Quatro previsões erradas nesta sessão — 19% que deu
47%, "grande" que deu 4%, 4x que deu ruído, e um ganho de FPS que deu zero — e
todas as que acertaram vieram de medição, nunca de leitura de código. O caminho
certo é contadores de GPU do Perfetto para saber em que estágio o tempo se
perde.

## Reprodução

- Perfil de CPU: `adb shell run-as org.renut.android /system/bin/simpleperf record -p <pid do :runtime> -g -f 1000 --duration 20 -o /data/data/org.renut.android/files/perf.data`
- Ocupação da GPU: `cat /sys/class/kgsl/kgsl-3d0/gpubusy` (ocupado e total desde a última leitura; `gpu_busy_percentage` zera na leitura e engana)
- Trace de um quadro: `touch files/plume-trace-request`, saída em `files/plume-trace.txt`
- Reverter por bit, sem recompilar: `echo <máscara> > files/plume-skip`, lido a cada 30 swaps.
  `1048576` volta a vincular os 32 slots de textura; `2097152` devolve ao Clear o
  poder de dimensionar os alvos.
