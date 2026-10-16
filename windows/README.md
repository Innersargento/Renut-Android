# reNut para PC (Windows) com o renderizador Plume

Executável de teste que roda o jogo recompilado no Windows x86-64 usando o mesmo
renderizador nativo do Android (Plume sobre Vulkan, com os shaders recompilados
offline pelo XenosRecomp). Não substitui o build desktop do upstream.

**RexGlue e Plume entram como submódulos Git, sem nenhuma alteração:**
`thirdparty/rexglue-sdk` (tag `v0.10.0`) e `thirdparty/plume` (revisão fixa).
Nada do que o reNut precisa a mais deles está num patch: fica no código do
próprio reNut (`src/`):

- o que o renderizador precisa do SDK sai de classes e ganchos que o SDK já
  expõe (a thread do command processor é a do SDK; o loop dela é nosso);
- as exportações do kernel que o reNut troca (ex.: `VdSwap`) são renomeadas na
  compilação do código do jogo (`guest/kernel_overrides.*`), sem tocar no SDK;
- o que o Plume não expõe (estado dinâmico de stencil/blend, formatos de vértice
  convertidos, cache de pipelines em disco) é feito numa camada Vulkan nossa,
  que troca os ponteiros de função do volk depois que o Plume cria a instância
  (`plume/plume_renderer.cpp`).

Estado (13/10/2026): compila, inicia, mostra a logo da Rare e a tela-título e
roda a ~30 FPS numa RTX 3060 por minutos sem cair. Não foi jogado além do menu,
não tem teclado (só controle, via XInput) e o áudio/entrada não foram revistos.

## O que é preciso

- Os submódulos: `git submodule update --init --recursive`.
- `generated/android` e `generated/shaders` (veja `docs/BUILD.md`: o código do
  jogo e os shaders saem do seu próprio disco, `tools/prepare.py`).
- Windows 10/11 com driver Vulkan (a GPU precisa de `vulkan-1.dll`).
- **Sem Visual Studio.** O compilador é o clang do NDK do Android (um clang 21
  que roda no Windows) apontado para o alvo MinGW-w64, com cabeçalhos e
  libstdc++ do MSYS2 UCRT64:
  - Android NDK 27+ instalado pelo Android Studio (`%LOCALAPPDATA%\Android\Sdk\ndk`);
  - MSYS2 em `C:\msys64` com `mingw-w64-ucrt-x86_64-gcc`, `cmake` e `ninja`.
    (`RENUT_NDK_BIN` e `-DRENUT_MSYS=` sobrescrevem os caminhos.)

## Compilar

```bash
export PATH="/c/msys64/ucrt64/bin:/c/Windows/System32"   # sem o git do Windows no PATH
cmake -S windows -B out/build/win-plume -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$PWD/windows/toolchain-ndk-clang.cmake"
cmake --build out/build/win-plume --target renut_plume_exe
```

A primeira vez leva uns 15 minutos (SDK, FFmpeg, glslang e o código do jogo).
O resultado é `out/build/win-plume/renut_plume.exe`, estático: não precisa de
DLLs do MSYS2.

## Executar

```
renut_plume.exe --game "<pasta com default.xex e Bundle>" [--data <pasta>] [--config <renut.toml>]
```

`--data` guarda log (`rexglue-runtime.log`), cache de pipelines e saves; o padrão
é `renut-data` ao lado do `.exe`. Os cvars de `renut.toml` são os do Android.

## Configuração, save e contador

- `renut.toml` na pasta de dados é lido na abertura. Taxa de quadros:
  `renut_fps_target = 30` é o ritmo original do jogo (preso ao vblank); de 31 a
  240 o jogo apresenta sem esperar o vblank e é mantido nessa taxa; `0` não tem
  limite. Acima de 60 (ou `0`) o Plume deixa de apresentar no vsync (Mailbox ou
  Immediate); `renut_plume_vsync = false` faz isso em qualquer taxa.
  `renut_plume_uncap_fps = true` ainda é lido e vale `renut_fps_target = 0`.
- O save é o do reNut desktop: copie o conteúdo de `Documents\renut` para
  `<pasta de dados>\runtime-user` (perfil `B13E...`, conquistas em `achievements`).
- O título da janela mostra os FPS e o número do quadro (quadros que o jogo
  apresentou, a cada meio segundo).
- `windows/jogar.cmd` abre o jogo com `out\win-plume-data`.

## Problema conhecido

Cerca de uma em cada seis aberturas cai alguns segundos depois: o jogo libera
blocos de um heap que já não está registrado (todos de uma vez, de duas threads),
e o `free` do alocador dele entra numa seção crítica nula. `sub_8223FD48` em
`guest/game_fixes.cpp` descarta esse free (vaza o bloco), o que evita essa queda, mas
em parte das vezes o jogo cai logo depois por outro motivo (pilha corrompida).
A causa não foi encontrada; o padrão aponta para uma corrida entre threads do
jogo ao descarregar uma cena, e não para o FPS liberado.

## O que difere do Android (e por quê)

| Ponto | Windows | Onde |
| --- | --- | --- |
| Toolchain | clang do NDK + MSYS2, sem MSVC | `toolchain-ndk-clang.cmake` |
| SDK | `rexruntime` vira biblioteca estática dentro do `.exe`; o plugin `rexgpu-xenos` não é usado, só alguns helpers (`windows/xenos-helpers.cmake`) | `CMakeLists.txt` (`renut_static_copy`) |
| Janela | HWND Win32 passado ao Plume | `platform/windows/main_windows.cpp`, `plume/plume_renderer.*` |
| Carregador Vulkan | `vulkan-1.dll` | `platform/windows/platform_windows.cpp` |
| Memória do guest | a vista `0xE0000000` fica 0x1000 acima; `GuestPtr()` aplica o deslocamento nas leituras diretas | `src/guest/guest_address.h` |
| Página zero | `protect_zero=false` (o jogo lê campos de ponteiros nulos) | `platform/windows/main_windows.cpp` |
| Ponteiros inválidos no device | `GuestCommitted()` antes de desreferenciar streams/superfícies/índices | `d3d/native/flush.cpp` |
| Canário de pilha | valor constante; o do libssp mudava sob uma thread que o SDK inicia em construtor estático ("stack smashing detected") | `platform/windows/platform_windows.cpp` |

## Armadilhas já resolvidas

- **Por que sem DLL:** o código gerado referencia as funções do kernel como
  `__imp__NtWaitForSingleObjectEx`; o linker COFF lê o prefixo `__imp_` como
  importação de DLL e o `export-all` do MinGW omite esses nomes.
- **Symlinks do SDK:** o git no Windows extrai `thirdparty/libmspack/cabextract/mspack`
  como arquivos de texto. O CMake usa os alvos reais (`libmspack/libmspack/mspack`).
- **Versão do SDK:** o SDK roda `git describe` no diretório do projeto; aqui isso
  pegaria as tags do próprio reNut. O CMake esconde o `git` do SDK.
- **VMA duplicado:** o Plume e a UI Vulkan do SDK definem a implementação do
  VulkanMemoryAllocator; o objeto do SDK sai do link.
