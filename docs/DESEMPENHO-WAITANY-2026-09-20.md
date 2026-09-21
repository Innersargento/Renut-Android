# RexGlue — a espera alertable girava um core por thread, 20/09/2026

Quarta etapa. Ver `DESEMPENHO-2026-09-20.md`, `DESEMPENHO-SPIN-2026-09-20.md` e
`DESEMPENHO-PIPELINE-2026-09-20.md`.

## O sintoma

Depois de corrigir o spin da fence e sobrepor CPU e GPU, a **Audio Worker**
passou a ser a maior consumidora de CPU do processo: 78-79% de um core, à frente
da GPU Commands. O processo inteiro somava 238%.

O perfil (simpleperf, 11.946 amostras) mostrou que **não era áudio**: o callback
do jogo era ~2%. 94,71% estavam em `rex::thread::WaitMultiple`, sendo 42,98% em
`mutex::try_lock`, 41,97% em `mutex::unlock` e 32,86% do total em
`__kernel_clock_gettime`.

## A causa

Em `src/core/threading_posix.cpp`:

```
WaitAny(..., is_alertable = true, timeout)
  └─ while(true): WaitMultiple(handles, wait_all, ComputeAlertableWaitTimeout())
                    kAlertablePollSlice = 1 ms
```

E dentro de `PosixConditionBase::WaitMultiple`:

```cpp
auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - now);
auto sleep_time = std::min(remaining, std::chrono::milliseconds(1));
std::this_thread::sleep_for(sleep_time);
```

Com a fatia de 1 ms que o invólucro passa, `end_time - now` vale ~998 µs na
primeira passagem, e **o cast para milissegundos trunca isso para zero**.
`sleep_for(0)` retorna na hora, o laço gira a fatia inteira checando handles,
devolve `kTimeout`, e o invólucro pede outra fatia. Um core por thread que
espere assim, para sempre.

`WaitAny` alertable é a primitiva de espera da camada RexGlue, não algo do
áudio.

## A correção

Calcular em microssegundos, com piso em zero. O laço cai de milhões de
iterações por segundo para ~1000.

Junto, e **sem relação com o ganho**: `WaitMultiple` para `wait_all = false`
deixou de travar todos os handles ao mesmo tempo. A trava do próprio handle já
torna atômicos a checagem e o consumo, e só um handle é consumido — enquanto o
caminho antigo desistia se um único estivesse ocupado e repetia com `yield()` sem
recuo nenhum. Isso foi medido e **não mudou nada** (78% antes e depois); é um
spin latente sob contenção, corrigido de passagem.

## O que rendeu

Mesma cena, `drawn` entre 1,23 e 1,28 milhões por 300 swaps, `passes` em 26700:

| | antes | depois |
| --- | --- | --- |
| Audio Worker | 78% de um core | **5%** |
| processo todo | 238% | 191% |
| ms por quadro | ~55-62 | **43,8 - 47,9** |
| FPS na tela | 16-18 | **20-23** |
| clock da GPU | 295-401 MHz (tpl 5-7) | **680 MHz (tpl 1)** |
| GPU ocupada | ~66% | 78% |

O ganho maior **não é a CPU liberada em si**: é o teto de clock da GPU subindo de
295-401 para 680 MHz, porque CPU e GPU dividem o mesmo orçamento de potência.
Segunda confirmação da hipótese de `plume-gpu-clock-cap`, desta vez com um salto
de 2,3x no clock.

## A sessão inteira, no mesmo trecho

| etapa | ms/quadro | FPS |
| --- | --- | --- |
| início | 152-153 | 6,6 |
| spin da fence bloqueando | 98,5-126 | 8-10 |
| CPU e GPU sobrepostas | ~55-62 | 16-18 |
| espera alertable corrigida | **43,8-47,9** | **20-23** |

**3,3x**, sem tocar em nada do que é desenhado.

## Onde o arquivo vive

`external/rexglue-android` é um clone fixado do rexglue-sdk e não é versionado
aqui; as adaptações ficam em `android/rexglue/android-platform.patch`, aplicado
por `tools/bootstrap-rexglue.ps1`. O patch foi regenerado com
`git -C external/rexglue-android diff` e confirmado reversível sobre a árvore
atual, então a correção sobrevive a um re-bootstrap.

## O que ainda está na mesa

- `WaitMultiple` ainda é um poll de 1 ms mesmo corrigido. Bloquear de verdade
  pede uma variável de condição compartilhada entre os handles — redesenho da
  camada. A ~1000 acordadas por segundo virou ruído, mas continua sendo trabalho
  à toa.
- **GPU Commands a 78% de um core** é agora a maior carga, e o `submit_ms` caiu
  para ~1 ms: o quadro é gravação de lista, não espera. O custo por passe de
  render continua sem instrumentação neste aparelho.
