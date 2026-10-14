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
  (`android/rexglue/`), usando o que o SDK e o Plume já expõem.
- Correções que valem para todo mundo (e não só para o reNut) devem ir para o
  projeto de origem como PR, e só voltar para cá quando forem aceitas.

```
git submodule update --init --recursive
```

## Como o reNut usa o SDK sem alterá-lo

1. **O renderizador é nosso, o command processor é o do SDK.**
   `PlumeCommandProcessor` (plume_gpu.cpp) deriva de `rex::graphics::CommandProcessor`.
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
- **Android: ainda no caminho antigo.** O APK ainda compila contra
  `external/` (pastas preparadas por `tools/prepare.py`) com
  `android/rexglue/android-platform.patch` e `android/plume-formats.patch`. A
  tabela abaixo diz o que cada parte do patch do SDK vira. Nada disso será
  aplicado quando o Android passar para os submódulos.

## O patch antigo do SDK, parte por parte

| O que o patch fazia | Destino |
| --- | --- |
| Ganchos do command processor (`OnPacketStart`, `OnIndirectBuffer`, trabalho nativo, `OnPredicatedSkip`, censo) | **Feito**: laço próprio na thread do SDK |
| `SetSingleBinTiling` | **Feito**: `renut::d3d::SetSingleBinTiling` |
| `GetResolveInfoFromRect` | **Feito**: o SDK recebe um arquivo de registradores que descreve o retângulo (`ResolveInfoForRect`) |
| `PresentFrontBuffer` e o gancho em `VdSwap` | **Feito**: `renut_imp_VdSwap` |
| Contadores e logs de diagnóstico (trabalho da GPU, pools, `WaitDiag`, traço de eventos de áudio, falha de MMIO, estatísticas do CP) | **Descartados**; o que serve vira medição do próprio reNut |
| Pausa do jogador (`XThread::SetTitlePaused`) | **Feito** em `host_support.cpp`; falta ligar nas esperas do kernel por override (Android) |
| Texto do teclado do jogo (`SetTextInputHandler`) | A fazer: override de `XamShowKeyboardUI` (Android) |
| Vários jogadores (`XamUser*`, `XamInput*`) | A fazer: overrides (Android) |
| `KeInitializeEvent` reaproveitando o objeto, `XamAlloc` com flags, handles de áudio inválidos, assert do spinlock | A fazer: overrides (Android) |
| `X_KTHREAD::kernel_time` avançando (vigia do alocador do jogo) | A fazer: thread do host (Android) |
| Fibras com libucontext, `chrono.h` no NDK | A fazer: cabeçalho incluído à força no arquivo (Android) |
| Espera em vários objetos sem busca ativa, `sigqueue` por thread no Android | **Sem substituto de fora**: PR no RexGlue |
| Ciclo de vida do cliente de áudio, prioridade das threads de áudio | Parte por override; resto, PR no RexGlue |
| Pacotes XMA sem quadro inicial | **Sem substituto de fora**: PR no RexGlue |
| `QueryProtect` por falta de página em Linux/Android | **Sem substituto de fora**: PR no RexGlue |
| Dobra estéreo do áudio no Android (SDL/AAudio) | A fazer: backend de áudio próprio do reNut (o SDK aceita um) |
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
