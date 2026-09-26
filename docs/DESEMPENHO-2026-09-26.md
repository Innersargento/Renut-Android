# Plume — o custo por desenho era dos shaders

26/09/2026. Samsung SM-S918B, Adreno 740, Turnip v26.3.0-R5, renderizador Plume.
Continua `DESEMPENHO-2026-09-21.md`. Todas as janelas foram lidas com o clock
da GPU amostrado (`/sys/class/kgsl/kgsl-3d0/clock_mhz`); o aparelho varia entre
220 e 719 MHz na mesma sessão, e as comparações abaixo são a clock igual.

## Resultado

| Showdown Town, ~5.200 desenhos | início | fim |
| --- | --- | --- |
| GPU por quadro | ~46 ms (8,9 µs/desenho) | ~34,5 ms (6,6 µs/desenho) |
| CPU da thread de gravação (`cp_cpu_ms`) | ~31-35 ms | ~27 ms |
| quadro | ~46-47 ms | ~36-37 ms |

Numa área pesada (~8.800 desenhos): GPU ~56 ms, CPU ~47 ms, quadro ~61 ms.

## O que achou o custo

`files/plume-trivial` (1 = VS e PS triviais, 2 = só PS trivial) mediu que ~70% da
GPU eram os shaders recompilados do jogo, num custo **por desenho**: 3 vértices
por desenho e scissor 1x1 quase não mudavam o tempo. Depois das correções:
base (estado + desenho) ~9 ms, VS ~14 ms, PS ~11 ms a 719 MHz.

## O que entrou

1. **Bug no XenosRecomp**: o vfetch marcava o registrador de destino como escrito
   antes de ver se o índice tinha sido escrito, então `r0 = vfetch(r0.x)` - o
   fetch mais comum - parecia indexado por registrador calculado e ia para
   `vfetchLive` (load BDA + `switch` de ~40 formatos). 800 de 874 VS usavam esse
   caminho; agora 293 (os que de fato calculam o índice).
2. **Constantes em uniform buffers**: as constantes do VS, do PS e o bloco
   compartilhado passaram de `vk::RawBufferLoad` para UBOs dinâmicos no set 4
   (montado em Vulkan cru e anexado ao layout do Plume). Leituras de offset
   constante vão para o banco de constantes, carregado pelo CP antes do
   desenho, em vez de loads globais no preamble. 8,0 → 6,8 µs/desenho.
3. **`tfetch2D` sem consulta de dimensões** quando o offset é zero (93% dos
   fetches): ~3%.
4. **Água**: a grade da tesselação é persistente e indexada (antes, dezenas de
   MB escritos por quadro na CPU e o shader de domínio 6x por ponto).
5. **CPU**: chave de pipeline num hash só; `LoadShader` com cache por quadro;
   escritas de constantes em bloco com marca de "escrito" (sem `memcmp` quando
   nada mudou); `FindResolved` memorizado; viewport, scissor, vertex e index
   buffers só quando mudam (o index buffer é o ring inteiro, com `firstIndex`).

## Descartado (não repetir)

- **`IR3_SHADER_DEBUG=nopreamble`**: o mais rápido (5,3 µs/desenho), mas placas
  penduradas (Jolly Joe's) alternam entre dois tons a cada quadro. Some com o
  preamble ligado. Bug do compilador no modo sem preamble.
- **`noearlypreamble`**: sem ganho claro sobre o padrão.
- **Manter o pipeline anterior** (plume-skip 262144) fica *mais lento* - os
  shaders errados fazem mais trabalho; não mede o custo da troca.

## Instrumentos novos

- Relatório de quadro: `cp_cpu_ms` (CPU real da thread), `slot_wait_ms`,
  `stream_mb`, `constant_mb`.
- `files/plume-env`: linhas `CHAVE=valor` no ambiente antes do driver carregar
  (o cache de pipelines fica separado por conteúdo).
- `files/plume-trivial`: shaders triviais, para separar VS, PS e base.

## Perfil da CPU na área pesada (thread GPU Commands)

`IssueDraw` 13% · cópia de streams (`CopySwapped` + `UploadGuest`) 14%,
~48 MB/quadro · constantes (`memmove` + `memcmp`) 14%, ~42 MB/quadro · XXH3 do
conteúdo de texturas 11% · parsing de pacotes ~8%.
