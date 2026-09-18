# Diagnóstico da parada na abertura — 17/09/2026

Continuação de `docs/RETOMADA-2026-09-13.md`. Branch `codex/android-xenos-runtime`.
Aparelho: SM-S918B, Android 16, Adreno 740. Importação de 6,5 GB preservada.

## Conclusão

A tela de peças douradas **não é um travamento gráfico nem espera de entrada**. A thread
principal do convidado gira a 100% de CPU dentro do alocador do próprio jogo e nunca sai.

Cadeia medida com `simpleperf` (3.988 amostras, 8 s, thread `Main XThread`):

```
gameMain → appMainTickPreDraw → sub_822B6770 → … → sub_82C30980
  → sub_82C37BB0                                   80,05%
       ├── meInternalAlloc                          57,80%
       └── meInternalFree                           42,11%
             └── sub_82287590 → sub_8228F558 → sub_8264DCD0
```

Distribuição plana: `sub_8264DCD0` 40,1%, `sub_8228F558` 35,1%,
`__restgprlr_29`/`__savegprlr_29` 18,5%. Ou seja, ~94% do tempo está em código
recompilado do jogo, não no emulador.

O laço está em `generated/android/renut_android_recomp.161.cpp`, em `sub_8228F558`:
monta um registro local em `r1+80` com o objeto do alocador, o tamanho pedido, o
ponteiro de heap, o descritor de thread (`r13+256`) e o valor de `mftb`
(`REX_QUERY_TIMEBASE`), e então repete `sub_8264DCD0` até ela devolver zero,
recarregando os ponteiros de heap (`r31+10896`, `r31+10908`) a cada volta. É um
laço de recuperação de memória que nunca consegue satisfazer a alocação.

## Evidências

- Retângulo do jogo **idêntico pixel a pixel** em três capturas ao longo de 9 s,
  enquanto a GPU continua apresentando. Confirma o alerta do recado anterior: swaps
  contínuos não são progresso visual.
- Nenhum shader ou pipeline novo desde 23:15:14; a sessão seguiu mais de 7 minutos
  só apresentando o mesmo quadro. Taxa caiu de ~26 para ~18,5 swaps/s.
- Estados de thread: `Main XThread` em `R` com ~102% de um núcleo; `Audio Worker`
  em ~82%; `GPU Commands` em ~24%; demais `XThread*` bloqueadas em futex.
- `NtQueryInformationFile(XFileXctdCompressionInformation)` ocorre 119 vezes mas
  **cessa às 23:14:55**, antes da parada (~23:15:13). Não é a causa. O retorno
  `X_STATUS_INVALID_PARAMETER` é igual ao do Xenia upstream.

## O que foi corrigido nesta sessão

1. **Log inutilizável.** O aviso de fetch constant inválido inundava o arquivo: a
   sessão anterior guardou só 11 s porque 2.372 de 2.377 linhas eram esse aviso, e a
   rotação apagou o início. `src/graphics/pipeline/texture/cache.cpp` agora reporta
   cada descritor distinto uma vez e emite um resumo periódico das repetições. Efeito
   medido: 2.394 linhas → 2, e uma sessão registrou "Suppressed 80000 repeated…".
   O comportamento não muda e `gpu_allow_invalid_fetch_constants` continua desligado.

2. **`QueueUserCallback` falhando sempre.** `src/core/threading_posix.cpp` chamava
   `sigqueue(tid, …)`, mas `sigqueue` endereça um processo, não uma thread: o tid
   mirava um pid alheio e falhava com ESRCH (`signal delivery failed (-1)`).
   Substituído pelo syscall `rt_tgsigqueueinfo`, que é o que a bionic usa em
   `pthread_sigqueue` (indisponível aqui porque exige API 29 e o build é minSdk 28).
   Verificado: 4 falhas na execução anterior, 0 depois. Isso **não** é a causa da
   parada — as esperas alertáveis fazem poll de 1 ms, então custava latência apenas.

3. **Build sem otimização.** `assembleDebug` deixa `CMAKE_BUILD_TYPE=Debug` e só o
   alvo `rexgpu-xenos` tinha `-O2`; `renut_guest` estava em `-O1` e todo o resto
   (`rexcore`, `rexruntime`, `rexkernel`, `rexfilesystem`, `rexaudio`, `rexui`,
   glslang, SPIRV-Tools, SDL3) em `-O0`, apesar de `REXGLUE_BUILD_CONFIG="Release"`.
   `android/rexglue/CMakeLists.txt` agora aplica `-O2` à árvore inteira e `renut_guest`
   subiu para `-O2`. `NDEBUG` **não** foi ativado, para manter asserts e comportamento.

## Ponto de atenção sobre o driver

As preferências apontam `libvulkan_freedreno.so`, mas o log da execução registra
`driverID: QualcommProprietary` / "Qualcomm Technologies Inc. Adreno Vulkan Driver".
O carregador adrenotools **não está aplicando o driver selecionado**. A execução
medida aqui usou o driver do sistema, igual à linha de base do recado anterior.

## Resultado do build otimizado: ganho real, parada mantida

| Métrica | `-O0`/`-O1` | `-O2` |
| --- | --- | --- |
| Arena mapeada → primeiro quadro | 2,62 s | 1,87 s (−29%) |
| Swaps/s durante o laço | 18,5 | 27,3 (+47%) |
| Avança além do laço | não | não |

O laço **não era lentidão**: é um impasse. Compilação completa levou 7,2 min.

## Hipótese testada e descartada: `kernel_time`

O jogo lê `*(r13+0x100)` (que `X_KPCR` confirma ser `X_KPRCB prcb_data` em 0x100, cujo
campo 0x0 é `current_thread`) e depois `+0x58` desse `X_KTHREAD`. O offset está
documentado em `src/kernel/xboxkrnl/xboxkrnl_threading.cpp:48` como `0x058: kernel time`,
mas era `uint8_t unk_58[0x4]` **sem uma única escrita em todo o SDK**.

Foi implementado: campo renomeado para `kernel_time` e uma thread "Kernel Time" no
`KernelState` o avança a cada 1 ms com `QueryGuestUptimeMillis()`, encerrada no destrutor.
A thread foi confirmada em execução no aparelho.

**Isso não destravou o jogo.** O perfil continua idêntico (`sub_8228F558` 41,8%,
`sub_8264DCD0` 37,7%). A correção é legítima — o campo era realmente não implementado —
mas não é a causa da parada. Mantida por ser comportamento correto.

## Onde a investigação parou

Releitura de `sub_8264DCD0` revela um segundo caminho de rearme que explica por que o
timeout é inalcançável:

```
bl sub_822402A8                      # id da thread atual (3,7% do perfil)
se *(r29+10888) == r3 e *(r29+11004) != 0:
    stw r30,12(r31)                  # rearma o watchdog
```

Enquanto esta thread for a dona do alocador, o watchdog se rearma indefinidamente. Ou
seja, **o laço espera progresso de outra thread**, não o vencimento de um prazo.

Medição de CPU por thread confirma o quadro: `Main XThread` em `R` (~102% de um núcleo),
`Audio Worker` em `R` (~82%), `GPU Commands` em `R` (~36%), e **7 `XThread*` do jogo com
zero ticks de CPU em 6 s**, todas bloqueadas em `futex_wait_queue_me`.

## Próximos passos, em ordem

1. **Principal: despertar perdido entre threads convidadas.** As 7 `XThread*` ociosas
   deveriam estar produzindo o progresso que o alocador espera. Identificar em que objeto
   de espera cada uma está parada e qual evento deveria sinalizá-las. O bug já corrigido
   do `sigqueue` era exatamente dessa família; procurar outros (entrega de APC,
   `KeSetEvent`, semáforos, conclusão de E/S assíncrona).
2. Descobrir o significado de `r29+10888` (dona do alocador) e `r29+11004`, e o retorno
   de `sub_822402A8`, para saber qual thread o jogo espera.
3. `Audio Worker` a ~82% de um núcleo continua desproporcional e não investigado.
4. Corrigir o carregador adrenotools ou remover a seleção de driver da UI: as
   preferências pedem `libvulkan_freedreno.so` e o log registra `QualcommProprietary`.
5. Apresentação em 1280×720 dentro de superfície mais larga, com tarjas laterais.
   Tratar depois do boot.

## Evidências desta sessão em `out/`

- `run041-runtime.log` (bruto; contém sessões anteriores — o log é acumulativo),
  `run041-meu.log` (execução das 23:15, 265 linhas legíveis).
- `run041-t20.png`, `run041-agora.png`, `run041-pos-start.png`, `frz-1..3.png`.
- `threads-frozen.txt`, `s1.txt`/`s2.txt` (amostras de `/proc/<pid>/task/*/stat`).
- Perfil no aparelho: `/data/data/org.renut.android/perf-main.data`.

## Como reproduzir o perfil

`RuntimeActivity` não é exportada; iniciar pela UI (`MainActivity`, botão
"Iniciar Banjo (experimental)"). Conferir os limites pelo dump da UI antes de
automatizar toques. Segurar os botões por ~400 ms (`input swipe x y x y 400`):
`input tap` é curto demais para o jogo amostrar a ~30 Hz.

```
adb shell run-as org.renut.android /system/bin/simpleperf record \
  -t <tid da Main XThread> -g -f 499 \
  -o /data/data/org.renut.android/perf-main.data --duration 8
adb shell run-as org.renut.android /system/bin/simpleperf report \
  -i /data/data/org.renut.android/perf-main.data -g --sort symbol --percent-limit 2
```

`simpleperf` só funciona via `run-as` (como shell dá "Permission denied").
