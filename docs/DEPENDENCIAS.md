# Dependências: RexGlue e Plume sem alterações

O reNut usa dois projetos de terceiros que **não são nossos e não são
alterados aqui**:

| Projeto | Onde | Revisão |
| --- | --- | --- |
| [RexGlue SDK](https://github.com/rexglue/rexglue-sdk) | `thirdparty/rexglue-sdk` (submódulo) | tag `v0.10.0` |
| [Plume](https://github.com/renderbag/plume) | `thirdparty/plume` (submódulo) | `d890ac8` |

Regras:

- Nada em `thirdparty/` é editado, corrigido por patch ou copiado para dentro do
  reNut. `git submodule status` mostra as revisões; qualquer diferença é erro.
- O que o reNut precisa a mais deles fica **no código do reNut**
  (`src/`), usando o que o SDK e o Plume já expõem.
- Correções que valem para todo mundo (e não só para o reNut) devem ir para o
  projeto de origem como PR, e só voltar para cá quando forem aceitas.

```
git submodule update --init --recursive
```

## Como o reNut usa o SDK sem alterá-lo

1. **O renderizador é nosso, o command processor é o do SDK.**
   `PlumeCommandProcessor` (plume/gpu/internal.h) deriva de `rex::graphics::CommandProcessor`.
   A thread do command processor é a que o SDK cria; ela chama `SetupContext()`
   (virtual) e é ali que roda o laço de trabalho do reNut (`RunWorkerLoop`), que
   faz o que o do SDK faz e, em volta de cada pacote do anel, o trabalho nativo
   (listas Direct3D, sincronização). Os pacotes em si são executados pelo SDK.
2. **Funções do jogo: `REX_HOOK_RAW`.** As 235 funções Direct3D do jogo
   (`d3d_functions.inc`, expandidas em `d3d_layer.cpp`) e as correções de jogo
   (`game_fixes.cpp`) substituem as definições fracas do código recompilado,
   exatamente como o `hooks.cpp` do reNut desktop.
3. **Exportações do kernel: renomeadas na compilação do jogo.** O código do jogo
   chama o kernel por `__imp__<Export>`. Para cada nome listado em
   `kernel_overrides.inc`, o CMake compila o código do jogo com
   `__imp__<Export>=renut_imp_<Export>` (`kernel_overrides.cmake`), e
   `kernel_overrides.cpp` define esse nome, chamando a função do SDK pelo nome
   verdadeiro quando não toma a chamada. É assim que `VdSwap` entrega o quadro
   às listas nativas.
4. **O que o Plume não expõe: uma camada Vulkan.** O Plume chama o Vulkan pelos
   ponteiros globais do volk. Depois que ele cria a instância, o reNut troca
   alguns (`InstallVulkanLayer`, plume_renderer.cpp):
   `vkCreateDevice` (liga `VK_EXT_pipeline_creation_cache_control`),
   `vkCreate{Graphics,Compute}Pipelines` (cache de pipelines em disco; estado
   dinâmico de stencil e de blend; formatos de vértice convertidos para float),
   `vkCreateBuffer` (uso indireto e endereço de dispositivo) e
   `vkQueueSubmit`/`vkWaitForFences` (perda do dispositivo). Cópia
   textura→buffer e os comandos de estado dinâmico saem direto no command
   buffer do Plume.
5. **Ajustes de compilação** (só `windows/CMakeLists.txt`, nunca nos fontes do
   SDK): o SDK vira biblioteca estática dentro do `.exe` (as exportações do
   kernel têm o prefixo `__imp_`, que o linker COFF lê como DLL), os symlinks do
   `mspack` são resolvidos para os alvos reais, o `git` fica escondido do SDK
   (ele lê a versão das tags do repositório) e um cabeçalho é incluído à força
   num arquivo do driver XInput.

## Estado por plataforma

- **Windows (PC): feito.** `windows/` compila e roda contra os dois submódulos
  intactos: logo da Rare, abertura e tela-título (renderização conferida por
  captura da própria janela), ~2.100 quadros em 65 s sem queda.
- **Android: compila e roda contra os dois submódulos intactos** (13/10/2026).
  `android/native/CMakeLists.txt` aponta para `thirdparty/rexglue-sdk` e o
  Plume é compilado de `thirdparty/plume`; `external/` só guarda o que não é
  SDK nem Plume (libadrenotools, libucontext). O que o Android precisa a mais
  vem de `src/platform/android/shims/` (cabeçalhos incluídos à força em arquivos do
  SDK), de `kernel_overrides.*` e do áudio próprio (`renut_audio.cpp`). Chega à
  tela-título; o que ainda falta está na tabela abaixo.

## O patch antigo do SDK, parte por parte

| O que o patch fazia | Destino |
| --- | --- |
| Ganchos do command processor (`OnPacketStart`, `OnIndirectBuffer`, trabalho nativo, `OnPredicatedSkip`, censo) | **Feito**: laço próprio na thread do SDK |
| `SetSingleBinTiling` | **Feito**: `renut::d3d::SetSingleBinTiling` |
| `GetResolveInfoFromRect` | **Feito**: o SDK recebe um arquivo de registradores que descreve o retângulo (`ResolveInfoForRect`) |
| `PresentFrontBuffer` e o gancho em `VdSwap` | **Feito**: `renut_imp_VdSwap` |
| Contadores e logs de diagnóstico (trabalho da GPU, pools, `WaitDiag`, traço de eventos de áudio, falha de MMIO, estatísticas do CP) | **Descartados**; o que serve vira medição do próprio reNut |
| Pausa do jogador (`XThread::SetTitlePaused`) | **Feito**: `host_support.cpp` + overrides das esperas, atrasos e `NtYieldExecution` |
| Texto do teclado do jogo (`SetTextInputHandler`) | **Feito**: override de `XamShowKeyboardUI` em `text_input_bridge.cpp` |
| Vários jogadores (`XamUser*`, `XamInput*`) | A fazer: overrides (Android); o jogador 1 (toque) funciona |
| `KeInitializeEvent` reaproveitando o objeto, `XamAlloc` com flags, handles de áudio inválidos, assert do spinlock | **Feito**: override de `KeInitializeEvent`; handles de áudio nos overrides do XAudio; os asserts do SDK ficam fora (`NDEBUG`, como o SDK é distribuído) |
| `X_KTHREAD::kernel_time` avançando (vigia do alocador do jogo) | **Feito**: thread do host (`StartKernelTime`) |
| Fibras com libucontext, `chrono.h` no NDK | **Feito**: `shims/android_ucontext.h`, `shims/android_chrono.h`, `shims/android_core.h` |
| Espera em vários objetos sem busca ativa, `sigqueue` por thread no Android | **Sem substituto de fora**: PR no RexGlue |
| Ciclo de vida do cliente de áudio, prioridade das threads de áudio | **Feito** no áudio próprio: o cliente só some depois que o callback do jogo termina; as threads de áudio e do XMA sobem de prioridade |
| Pacotes XMA sem quadro inicial | **Sem substituto de fora**: PR no RexGlue |
| `QueryProtect` por falta de página em Linux/Android | **Sem substituto de fora**: PR no RexGlue |
| Dobra estéreo do áudio no Android (SDL/AAudio) | **Feito**: `renut_audio.cpp` (AAudio, estéreo, NEON); ver abaixo |
| Espera do `WAIT_REG_MEM` em 500 µs | A fazer: tratado no laço próprio, se a medição pedir |

## O patch antigo do Plume, parte por parte

| O que o patch fazia | Destino |
| --- | --- |
| Estado dinâmico de stencil e de cor de blend | **Feito**: camada Vulkan (`vkCreateGraphicsPipelines`) + `vkCmdSet*` no command buffer |
| Formatos de vértice convertidos (`USCALED`, `SSCALED`, 2_10_10_10) | **Feito**: pedidos como os formatos inteiros de mesma largura, trocados na camada |
| Buffers para desenho indireto e endereço de dispositivo | **Feito**: camada (`vkCreateBuffer`) |
| Cache de pipelines em disco e consulta sem compilar | **Feito**: camada (`vkCreate*Pipelines`, `vkCreateDevice`) |
| Cópia textura→buffer | **Feito**: `vkCmdCopyImageToBuffer` no command buffer |
| `createGraphicsPipeline` devolvendo nulo em falha | **Feito**: conferido pelo reNut (`CreateGraphicsPipeline`) |
| `textureCompressionBC` nas capacidades | **Feito**: consultado ao driver |
| Rastreio de memória, mapeamento persistente, sem compressão | **Descartados** (eram ferramentas de investigação) |
| Carregador Vulkan do app Android (driver embutido) | A fazer (Android): a abertura do `libvulkan` do Plume é redirecionada na compilação do arquivo, sem editá-lo |

## Áudio do Android (`src/platform/android/renut_audio.cpp`)

O backend SDL do SDK e o laço de trabalho do `AudioSystem` do SDK não são
usados no Android. O reNut registra o seu próprio `AudioSystem` em
`RuntimeConfig::audio_factory` e as quatro exportações `XAudio*` que lidam com
clientes de renderização passam por `kernel_overrides.cpp`:

- **Uma saída AAudio por cliente**: `LOW_LATENCY`, compartilhada, `float` estéreo,
  48 kHz, buffer de dois bursts (4 ms no aparelho de teste) que só cresce quando
  o aparelho informa um underrun.
- **O callback do aparelho só copia**: a dobra 5.1 → estéreo (NEON, mesma conta
  do `conversion.h` do SDK) roda na thread que entrega o quadro, em um anel SPSC
  de 64 quadros sem trava; o callback devolve créditos, não toma lock nem aloca.
- **Thread de trabalho própria**: dorme em um único futex que cada crédito
  devolvido acorda, em vez de esperar em todos os semáforos de uma vez (a espera
  em vários objetos do SDK em POSIX/Android gira e queima um núcleo). Sobe a
  thread do jogo e a do decodificador XMA para nice -10.
- **Ciclo de vida**: `XAudioUnregisterRenderDriverClient` só volta depois que o
  callback do jogo em execução terminou (o Banjo depende disso para encerrar a
  thread de renderização); handles que não são nossos são ignorados; fluxo
  desconectado (fone, troca de rota) é reaberto pela thread de trabalho.
- **Pausa**: o `Pause()`/`Resume()` do `AudioSystem` do SDK continuam valendo;
  as saídas AAudio entram em pausa de verdade.
- **Diagnóstico**: abertura, underruns e erros vão para o logcat (tag
  `renut-audio`), mesmo com o log do runtime desligado; quadros de silêncio por
  5 s saem na mesma tag e em `renut_audio_silent_frames()`.
