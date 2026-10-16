# src/ — o código do reNut

Tudo o que o reNut escreve fica aqui, e **o build do PC e o do Android compilam
exatamente estes arquivos**. O que é de uma plataforma só fica em
`platform/<plataforma>/`; o resto é comum.

Os dois builds definem `src/` como raiz de includes: um arquivo inclui outro
pelo caminho a partir daqui (`#include "d3d/d3d_layer.h"`).

```
src/
├── guest/      O jogo recompilado e as costuras com o kernel emulado
├── d3d/        Direct3D do XDK: interceptação e front end nativo
├── plume/      Renderizador nativo sobre Plume/Vulkan e seus shaders
├── vendor/     Código de terceiros copiado inteiro (smol-v)
├── platform/
│   ├── android/  JNI, entrada por toque, áudio AAudio, janela, ADPF, conquistas
│   └── windows/  main (janela Win32), carregador Vulkan, canário de pilha
```

## guest/

| Arquivo | O que faz |
|---|---|
| `kernel_overrides.{cpp,inc,cmake}` | Exportações do kernel que o reNut assume (`VdSwap`...). O `.inc` lista; o `.cmake` renomeia as chamadas na compilação do jogo |
| `game_fixes.cpp` | Substituições específicas do jogo: definições fortes que trocam funções recompiladas (símbolos fracos) |
| `cheats.cpp` | Ganchos de cheat que o código gerado chama em endereços fixos (`config/renut_android_hooks.toml`), ligados por cvar |
| `host_support.{h,cpp}` | Pausa do jogador e contadores compartilhados entre renderizador e contador de quadros |
| `guest_address.h` | Conversão endereço do guest → ponteiro do host |
| `renut_guest_pch.h`, `renut_simd_fast.h` | Cabeçalho pré-compilado do código do jogo e as tabelas de byte swap vetorial |

## d3d/

| Arquivo | O que faz |
|---|---|
| `d3d_functions.inc` | Lista (X-macro) das entradas do Direct3D do XDK que o jogo chama; dá o enum `Function` |
| `d3d_hooks.inc` | Um `REX_HOOK_RAW` por função, agrupado em *Taken* (o front end nativo assume), *Observed* (o original roda, o reNut lê ou ajusta) e *Forwarded* (só contada) |
| `d3d_layer.{h,cpp}` | `Enter`/`Leave` comuns: conta, encaminha ao original ou entrega a um handler |
| `plume_d3d.cpp` | Handlers do modo Plume |
| `native_d3d.h` | API do front end D3D nativo (listas e sincronização sem PM4/EDRAM); a implementação está em `native/` |

### d3d/native/

O front end nativo, dividido por assunto (tudo em `renut::native_d3d`; `internal.h` guarda os tipos e o estado compartilhado).

| Arquivo | O que faz |
|---|---|
| `internal.h` | Tipos, contadores, listas, memória de rascunho e declarações |
| `lists.cpp` | Listas de gravação, publicação |
| `flush.cpp` | O flush de estado (`SetPending_*` refeito a partir do device) |
| `draws.cpp` | Draws, clears e resolves |
| `commands.cpp` | Command buffers, predicação, clip de superfície, Swap |
| `sync.cpp` | O que a GPU escreve de volta para a CPU: fences, callbacks, espera do swap |
| `entry.cpp` | Pontos de entrada públicos (`OnEnter`/`OnLeave`...) |
| `report.cpp` | O relatório em `files/native-d3d.txt` |

## plume/

| Arquivo | O que faz |
|---|---|
| `plume_gpu.h` | API pública do sistema gráfico (iniciar, parar, cena e pipelines); a implementação está em `gpu/` |
| `plume_renderer.{h,cpp}` | Dono da janela/swapchain via Plume; alimentado pela camada D3D; camada Vulkan própria (estado dinâmico, cache de pipelines) |
| `plume_textures.{h,cpp}` | Decodifica no CPU as texturas do jogo (destile, byte swap, RGBA8) |
| `spirv_rewrites.h`, `xenos_ucode_key.h`, `shader_cache.h`, `shader_cache_blob.S` | Transformações de SPIR-V, chave do microcódigo, cache de shaders embutido |
| `shaders/` | Shaders próprios do renderizador (`.vert/.frag/.comp/.geom` e o `.inc` com o SPIR-V; regerados por `tools/shaders/build_host_shaders.sh`) |

### plume/gpu/

O processador de comandos do Plume, dividido por assunto. Tudo em `renut::gpu`; `internal.h` inclui, em cadeia, `base.h` (includes e auxiliares), `shader_store.h`, `recipes.h`, `formats.h`, `resources.h`, `types.h` (tipos e constantes do backend) e `backend.h`. O `GpuBackend` é só o dono: o trabalho está em subsistemas, uma struct por assunto (`targets.h`, `textures.h`, `pipelines.h`, `streams.h`, `shaders.h`, `lists.h`, `draw.h`, `recorder.h`, `hiz.h`, `resolve.h`, `patches.h`, `swap.h`, `profiling.h`, `report.h`), cada um com o seu estado e os seus métodos nos `.cpp` de mesmo nome. Um subsistema alcança os outros pela referência `backend`. Nada fora de `gpu/` os inclui.

| Arquivo | O que faz |
|---|---|
| `internal.h` | Índice: inclui os cabeçalhos acima |
| `options.cpp` | Os cvars `renut_plume_*` |
| `support.cpp` | Funções livres: formatos, blend, entradas de shader, receitas, mapa de microcódigo |
| `pipeline_cache.cpp` | Threads de pipelines, arquivo de receitas, predição de pipelines |
| `context.cpp` | Contexto Vulkan (criar e desfazer) e o laço do worker |
| `streams.cpp` | Anel de upload, streams de vértice mantidos, memexport, blocos de constantes |
| `profiling.cpp` | Timestamps de GPU e o profiler de pares |
| `targets.cpp` | Início do quadro, render targets, framebuffers, imagens resolvidas |
| `textures.cpp` | Texturas do jogo e samplers |
| `shaders.cpp` | Escrita de registradores, busca de shaders, estatísticas de pipeline |
| `native_exec.cpp` | Executa as listas nativas que o front end D3D grava |
| `draw.cpp` | `IssueDraw`: um draw do jogo, do estado dos registradores até o draw gravado |
| `recorder.cpp` | Thread de gravação, draws adiados, relatório de device perdido |
| `hiz.cpp` | Culling Hi-Z |
| `resolve.cpp` | Resolves e cópias de profundidade |
| `patches.cpp` | Expansão dos patches de tesselação |
| `swap.cpp` | `IssueSwap`: a apresentação do quadro |
| `report.cpp` | Relatórios de memória e estatísticas |
| `host.cpp` | `PlumeGpu` (o hardware de vídeo hospedado) e a API de `plume_gpu.h` |

## Onde ficam os scripts de build

- `android/native/CMakeLists.txt` — biblioteca nativa do APK (chamado por `android/app`).
- `windows/CMakeLists.txt` — `renut_plume.exe`.
- `generated/` — saída local do codegen e dos shaders (fora do Git).
