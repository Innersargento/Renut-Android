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
└── renut_engine/, main.cpp, renut_app.h
              O app desktop original (SDK RexGlue + codegen), fora dos dois builds acima
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
| `native_d3d.{h,cpp}` | Front end D3D nativo: listas e sincronização sem PM4/EDRAM |

## plume/

| Arquivo | O que faz |
|---|---|
| `plume_gpu.{h,cpp}` | Sistema gráfico: lê o PM4 do jogo e executa cada draw, resolve e swap com Plume e os shaders recompilados offline |
| `plume_renderer.{h,cpp}` | Dono da janela/swapchain via Plume; alimentado pela camada D3D; camada Vulkan própria (estado dinâmico, cache de pipelines) |
| `plume_textures.{h,cpp}` | Decodifica no CPU as texturas do jogo (destile, byte swap, RGBA8) |
| `spirv_rewrites.h`, `xenos_ucode_key.h`, `shader_cache.h`, `shader_cache_blob.S` | Transformações de SPIR-V, chave do microcódigo, cache de shaders embutido |
| `shaders/` | Shaders próprios do renderizador (`.vert/.frag/.comp/.geom` e o `.inc` com o SPIR-V; regerados por `tools/shaders/build_host_shaders.sh`) |

## Onde ficam os scripts de build

- `android/native/CMakeLists.txt` — biblioteca nativa do APK (chamado por `android/app`).
- `windows/CMakeLists.txt` — `renut_plume.exe`.
- `generated/` — saída local do codegen e dos shaders (fora do Git).
