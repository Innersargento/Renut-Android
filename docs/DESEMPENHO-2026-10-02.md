# Camada de CPU e lista de desenhos adiada

02/10/2026. Continua `DESEMPENHO-2026-09-26.md`. Nada disto foi medido no
aparelho ainda: o que está abaixo é o que o código e o binário mostram, e o que
falta medir está dito em cada item.

## Por que estas duas

Nas janelas de 27/09 (MSAA desligado, ~5.500 desenhos) a GPU levava 37,8 ms, o
quadro 39,5 ms e a thread de comandos 28,5 ms. A GPU custa ~3.100 a 3.400 ciclos
de clock por desenho (`1,45 µs + 2767/MHz`, R² 0,99 em 6 janelas): o tempo
acompanha o clock e o clock sustentado ficou entre 220 e 475 MHz. Calor e
consumo é o que decide o clock, então CPU a menos e GPU a menos valem mais do
que parecem.

No quadro de 5.728 desenhos de `out/measure-2026-09-27/scene-trace.txt`: 2.099
desenhos de sombra (4 vertex shaders, só 237 trocas de pipeline) e 3.011 na cena
principal com 2.265 trocas de pipeline (75% dos desenhos trocam de programa).

## 1. Lista de desenhos adiada (`plume_gpu.cpp`)

`IssueDraw` juntava o estado e gravava o Vulkan na hora. Agora monta um pacote
(`DrawPacket`: pipeline, offsets de constantes, viewport, scissor, vertex e index
buffers, argumentos do desenho) e:

- um desenho que **pode ser reordenado sem mudar a imagem** entra numa janela e é
  gravado, agrupado por pipeline, quando a janela fecha (`FlushDeferred`);
- qualquer outro desenho fecha a janela e é gravado na hora (`EmitDraw`).

**Ordem preservada.** Todo uso da lista de comandos passa por `L()`, que fecha a
janela antes. Barreiras, cópias, upload de textura, resolve, clear, exportação de
memória, `Flush` do anel e `IssueSwap` ficam exatamente onde estavam. O stream
de comandos só difere dentro de uma janela. Os pacotes só guardam valores já
copiados: nada em `EmitDraw` lê o arquivo de registradores.

**Quem pode esperar.** Desenho com teste e escrita de profundidade em direção
estrita (menor, menor-ou-igual, maior, maior-ou-igual), sem memexport, sem patch,
sem bandas mescladas e sem nenhuma chave de depuração. Stencil só se nunca ler
(função ALWAYS, falha e z-falha KEEP, z-passa KEEP/ZERO/REPLACE). A janela só
continua enquanto framebuffer, máscara de cor, estado de profundidade e stencil
(máscara de escrita) forem os mesmos.

**Modos** (`files/plume-defer`, relido a cada ~30 quadros; cvar
`renut_plume_defer_draws`; padrão 1):

| modo | quem entra | exatidão |
| --- | --- | --- |
| 0 | ninguém | igual a antes |
| 1 | só desenhos de profundidade (mapas de sombra) | exata: o valor gravado é o mesmo em qualquer ordem |
| 2 | também desenhos de cor opacos | exata, **exceto empates exatos de profundidade** entre desenhos de shaders diferentes (geometria coplanar) |

O modo 2 é onde o ganho está (cena principal: 2.265 → ~1.487 trocas de pipeline,
se o stencil real do jogo permitir) e **ainda não foi olhado numa tela**. Testar:
`echo 2 > files/plume-defer` e conferir a imagem no Showdown Town e na água; a
linha `Plume deferred draws (mode N): X draws in Y windows, pipeline changes A ->
B` do relatório mostra o efeito, e `pipeline_binds` do relatório de quadro cai.

O agrupamento (contagem por primeira aparição, estável) foi testado fora do
jogo com 20.000 sequências aleatórias: permutação, grupos contíguos, ordem
interna preservada, trocas nunca aumentam.

## 2. Camada de CPU

### a) Chamadas sem PLT (`CMakeLists.txt`, `-Wl,-Bsymbolic-functions`)

`librenut_runtime.so` tinha **24.648 slots de PLT, 23.839 deles para funções do
jogo** e para os helpers `__savegprlr_N` / `__restgprlr_N` que todo prólogo e
epílogo chama: funções fracas de visibilidade padrão numa biblioteca
compartilhada, que o linker tem de supor substituíveis. Cada chamada do jogo
virava até três saltos indiretos por stub. Com a ligação local, viram `bl`
comuns. Nada fora da biblioteca substitui essas funções (os overrides de
`game_fixes.cpp` são resolvidos na ligação estática).

### b) Comparações vetoriais em uma instrução (`renut_simd_fast.h`)

O código do jogo é compilado com `-ffp-model=strict`, que o ambiente de FP do
guest exige (ele troca flush-to-zero e arredondamento com `msr fpcr` entre
instruções de FP). Efeito colateral: o clang transforma comparações vetoriais de
FP em intrínsecos "constrained", e o backend AArch64 as **escalariza**. Medido
compilando o SIMDe e os helpers de `rex/ppc/intrinsics.h` com o clang do NDK
(`-O2 -ffp-model=strict`), em instruções da função de teste inteira, com as
duas cargas, o store e o `ret`:

| operação | antes | depois |
| --- | --- | --- |
| `cmpeq/gt/ge/lt/le_ps` | 22 | 5 |
| `max_ps` / `min_ps` | 23 / 27 | 6 |
| `vctsxs` (float → int saturado) | 43 | 5 |
| `vctuxs` | 127 | 5 |

O cabeçalho emite a instrução NEON direta em `asm volatile`. Ser `volatile`
importa: mantém a ordem em relação aos `msr fpcr` (também `volatile`), que é
tudo o que o modo strict protegia. A semântica é a do SSE original
(`max` = `a > b ? a : b`; NaN torna falsa qualquer relação ordenada) e, para as
conversões, a saturação com NaN → 0 que o código x86 emulava à mão.

Também troca as quatro tabelas de máscara (`VectorMaskL/R`, `VectorShiftTableL/R`)
por cópias `static const` locais a cada unidade. As originais são arrays mutáveis
exportados: todo acesso passava pelo GOT, era recarregado depois de cada
escrita e a máscara era refeita com `and` a cada uso (61.530 usos de
`VectorMaskL` no código gerado). Como constantes, o endereço é relativo ao PC e
sai do laço. O CMake copia as tabelas de `rex/ppc/intrinsics.h` na
configuração, então uma atualização do SDK não deixa cópia velha.

O cabeçalho entra no fim do PCH do jogo (`renut_guest_pch.h` embrulha o PCH
gerado), depois dos cabeçalhos do SDK, então só o código gerado depois dele é
afetado.

### Não feito, de propósito

- **`-ffp-model=precise` no código do jogo**: tiraria o custo do strict em todo
  o FP escalar, mas deixaria o compilador mover operações de FP através dos
  `msr fpcr`. Mudança numérica possível; fica para quando houver como comparar
  resultados.
- **LTO, PGO, `-march`**: build muito mais longo ou depende de rodar no
  aparelho, ou muda o conjunto de instruções.

## Medido no aparelho (02/10/2026, área pesada, SM-S918B)

O jogo estava com `log_level = "off"` (configuração do app), então o relatório do
Plume (`pipeline_binds`, linha `Plume deferred draws`) não existe nesta sessão.
Medi sem reabrir: nove janelas de 30 s alternando `files/plume-defer` (0, 1, 2,
2, 1, 0, 1, 2, 0), com o contador de quadros da camada do jogo no SurfaceFlinger
(`dumpsys SurfaceFlinger`, `Layer [id]`, campo `frame=`) a cada segundo, mais
`clock_mhz` e `gpu_busy_percentage`. Mais três gravações de 20 s do `simpleperf`
(`run-as`, o APK é debug; `cpu-clock`, sem amostras de kernel) uma por modo.

| modo | janelas | fps médio | clock médio | fps a 400 MHz |
| --- | --- | --- | --- | --- |
| 0 | 3 | 25,50 | 383 MHz | 26,75 |
| 1 | 3 | 25,81 | 383 MHz | 26,98 |
| 2 | 3 | 25,89 | 378 MHz | 27,42 |

- A GPU ficou em 98-99% ocupada o tempo todo, com o clock entre 348 e 550 MHz e o
  nível de potência subindo de 4 para 6 durante a medição (o teto desceu sozinho
  de 475 para 348 MHz). Por isso a comparação é feita contra o clock, não contra o
  tempo.
- Ajuste dos nove pontos: quadro = 20,9 ms + 6,82 × (1000/MHz) ms, R² 0,85. A
  400 MHz são 38 ms, dos quais 17 ms (45%) acompanham o clock da GPU.
- Resíduo por modo contra esse ajuste: modo 0 +0,38 ms, modo 1 −0,01 ms, modo 2
  −0,37 ms, com desvio geral de 0,48 ms e três janelas por modo. A ordem é a
  esperada, mas a diferença (~0,75 ms de 38, ~2%) está no limite do ruído. **Não
  há ganho de FPS que se possa afirmar com estas janelas.**
- O código novo rodou: no `simpleperf`, `FlushDeferred` tem 22, 44 e 99 amostras
  nos modos 0, 1 e 2 (no modo 0 é só a chamada com fila vazia) e `EmitDraw` cai
  de 210 para 170.
- Thread de comandos (`GPU Commands`): ~19.500-20.000 amostras em 20 s nos três
  modos, ou seja, ~0,65 núcleo. O driver Vulkan nela fica em 4.099 / 4.206 /
  3.711 amostras (modos 0 / 1 / 2): -10% no modo 2, ~0,013 núcleo, sem peso para
  consumo ou calor. O custo da ordenação no modo 2 (`FlushDeferred`) é 0,05
  núcleo.
- A leitura de CPU por thread via `/proc/<pid>/task/*/stat` que usei primeiro
  estava errada: os nomes `GPU Commands (...)` e `Main XThread (...)` têm espaço e
  deslocam os campos. Vale só o `simpleperf`.

### Preço dos desenhos de sombra (mesmo dia, mesma cena)

`files/plume-bind` = 16 derruba os desenhos só de profundidade (os casters de
sombra), `plume-bind` = 0 os deixa. Sete janelas alternadas de ~31 s (a primeira
rodada falhou: o jogo estava em segundo plano, GPU a 1%; refeita). Seis janelas
válidas, clock 295-360 MHz, nível de potência 6-7, GPU 99% ocupada:

| janela | plume-bind | fps | quadro | clock |
| --- | --- | --- | --- | --- |
| s1 | 0 | 19,34 | 51,7 ms | 297 MHz |
| s2 | 16 | 22,37 | 44,7 ms | 308 MHz |
| s3 | 0 | 22,50 | 44,4 ms | 360 MHz |
| s4 | 16 | 24,49 | 40,8 ms | 346 MHz |
| s5 | 0 | 22,02 | 45,4 ms | 348 MHz |
| s7 | 0 | 19,16 | 52,2 ms | 295 MHz |

Ajuste: quadro = 10,1 ms + 12,35 × (1000/MHz) ms − 5,3 ms sem sombras (desvio 0,19
ms, 6 pontos e 3 parâmetros: a ordem de grandeza vale, o decimal não). **As
sombras custam ~5 ms, ~13% do quadro.** Os ~87% restantes são a cena principal e
as passadas de tela. Nesta cena o quadro está 10 ms acima do de 400 MHz medido
antes (38 ms): o aparelho caiu para 295-360 MHz e a cena é mais pesada.

### O que isto muda

- A hipótese de que 75% de desenhos trocando de programa seriam o custo dominante
  da GPU **não se confirma** nesta cena: reduzir as trocas (modo 2) não mexeu no
  tempo de quadro de forma mensurável. 55% do quadro não depende do clock da GPU
  (parte fixa por desenho, ~21 ms), e a lista adiada não toca nessa parte.
- Falta saber quanto o modo 2 realmente reduz as trocas na prática (a estimativa
  de 2.265 → ~1.487 vem do rastro de 27/09, antes de decidir o que o stencil real
  permite). Isso só sai do relatório do Plume: ligar `log_level = "info"` nas
  configurações e reabrir o jogo.
- A camada de CPU não foi comparada com o build anterior no aparelho (exigiria
  instalar o APK velho). Contagem de instruções menor não equivale a ganho medido.
- Modo 2 numa tela: se algo errado apareceu nas janelas 3, 4 e 8, o modo 2 não
  deve virar padrão; o padrão continua 1.

## Perfil por par de shaders e a tabela de busca de vértice (02/10/2026, tarde)

### Perfil (`files/plume-pairprof`)

Modo de depuração em `plume_gpu.cpp` (`PairBegin`/`PairStamp`/`WritePairReport`):
`echo '2 3' > files/plume-pairprof` captura 3 quadros com dois timestamps em volta
de cada desenho e escreve `files/plume-pairprof.txt` (funciona com o log desligado).
Modo 1 carimba o topo do pipe (relógio do processador de comandos); modo 2, o fim
(cada desenho isolado). **Vale o modo 2**: no modo 1 a espera de drenagem de uma
passada cai no primeiro desenho da seguinte (um PS de 6 instruções apareceu com 1,9
ms). Três capturas do modo 1 concordam em 95-100% do top 20.

Cena de ~3.500-3.800 desenhos (modo 2): o custo por desenho **não** acompanha as
instruções do pixel shader (R² 0), nem as trocas de pipeline (<1 µs cada), nem os
vértices. Acompanha o **vertex shader** (instruções R² 0,53; leituras de memória
do VS R² 0,62, ~0,24 µs cada): VS sem busca "viva" 11-15 µs por desenho, VS com
busca viva (vfetchLive) 25-34 µs.

### Causa

Cada busca viva lê sua entrada (stream, formato, passo, deslocamento, escala) da
tabela do bloco uniforme frio (set 4, binding 9, índice 32+slot), faz um `switch`
de ~40 formatos sobre esse valor, e só então o `ldg`. Para o compilador do driver o
valor é opaco: cada ponto de busca vira uma cadeia de leituras dependentes que não
se sobrepõe às outras. Só que a tabela é fixa por shader (`HostShader::live_fetches`,
decodificada das instruções `vfetch`).

### Mudança (`spirv_fetch_spec.h`, `files/plume-fetchspec`)

O SPIR-V do VS (e do domínio da água) é reescrito uma vez: cada `OpLoad` da tabela
vira `OpCompositeConstruct` de quatro `OpSpecConstant` (ids 64 + slot×4 + palavra),
e `IssueDraw` lista as palavras como constantes de especialização do pipeline
(zero, o padrão, não entra). O driver dobra o `switch`. Não cria pipelines por
shader; o interruptor entra na chave. 298 dos 4.889 módulos foram reescritos (683
leituras) e todos passam no `spirv-val`. `files/plume-fetchspec` 0/1 ou a cvar
`renut_plume_fetch_spec` (padrão 1, ligado).

### Medido no aparelho (mesma cena, interruptor alternado ao vivo)

- Perfil, modo 2, mesmos desenhos precificados antes/depois: **45,2 → 30,2 ms (−33%)**
  de tempo somado de desenho. Por par: A017E382/42EAEDBF 30 → 15 µs (−51%),
  AAD7CBC5/2000375D 30 → 12 µs (−61%), D0F3B9AD/90FB601D 21 → 12 µs (−44%); até os
  VS de sombra caíram 16-34%.
- FPS, oito janelas de 25 s alternadas, GPU 99% ocupada, clock 263-328 MHz:
  **21,0 → 22,9 FPS (+9%)**, apesar de o clock médio ser menor com o interruptor ligado
  (287 contra 305 MHz). Ajuste quadro = 14,8 ms + 10,0 × (1000/MHz) ms − 6,2 ms
  ligado (desvio 0,71 ms): **a 400 MHz 39,8 → 33,7 ms, ~15% do quadro**.
- Os ~245 desenhos a menos vistos no perfil ligado eram a cena mudando: ao voltar a 0
  na mesma hora os desenhos ficaram em 3.597 contra 3.513-3.523.

### O que sobrou no topo (interruptor ligado, modo 2)

1. F09B139E/3277104C: 4 desenhos por quadro, 330 mil vértices cada, 691 µs cada =
   **6,9% do tempo de desenho**. É a tesselação da água (4 `patch_draws` por quadro,
   1,32 milhão de vértices): domínio do jogo rodando por ponto gerado pelo Plume. Baixar
   o nível da grade é a alavanca (custo visual na malha da água, não testado).
2. Os quatro VS de sombra (0988D51D, 12E7F917, 5053F7DC, 2A40203B): ~20% do tempo de
   desenho, 6-14 µs por desenho, perto do piso medido (~5 µs).
3. A017E382, D0F3B9AD, AA574274 e outros VS com busca viva continuam em 13-18 µs
   por desenho (antes 21-34).

Não verificado: a imagem com o interruptor ligado em todas as áreas do jogo
(personagens com esqueleto, água, sombras). O resultado deveria ser idêntico pixel a
pixel; o padrão é 1 (ligado) a pedido, e `files/plume-fetchspec` = 0 desliga.

## Modo 2 vira o padrão (11/10/2026)

`renut_plume_defer_draws` passa de 1 para 2: os desenhos opacos de cor com teste e
escrita de profundidade estritos também são reordenados por pipeline.

Medido no aparelho com cooler, modo desempenho e FPS liberado (Showdown à noite,
personagem parado, `tools/quick-fps.py --file plume-defer --states 0,1,2 --rounds 4`,
a ordem dos modos se inverte a cada rodada):

| modo | trocas de pipeline por quadro | FPS | GPU, Mcycles por quadro |
| --- | --- | --- | --- |
| 0 | ~4.160 | 32,42 | 14,70 |
| 1 | ~3.420 (-18%) | 32,06 | 14,75 (+0,3%) |
| 2 | ~2.640 (-37%) | 32,56 | 14,26 (-3,0%) |

- O modo 2 ficou abaixo do 0 em Mcycles nas quatro rodadas (-2,6, -2,5, -0,4, -6,1%).
  O FPS sobe 0,4%, dentro do ruído: o ganho é de ~3% de GPU, não de 30%. Os ~30% da
  análise de 02/10 eram de trocas de pipeline, não de tempo de quadro.
- Imagem: capturas 0/2/0/2/0 na mesma cena. Diferença 0×2 de 2,1-2,8% dos pixels
  (limiar de 3 níveis), a mesma faixa de 0×0 (2,2-3,0%) e 2×2 (2,1%). Nenhuma região
  nova. Com limiar de 16 níveis a diferença é de 0,02%; a cena é escura, então o
  limiar baixo é o que vale.
- `plume-defer` = 0/1/2 continua valendo ao vivo. `quick-fps.py` escreve 0 no
  arquivo ao terminar, o que força o modo 0: apague o arquivo depois de cada rodada.
