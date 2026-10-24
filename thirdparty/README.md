# thirdparty/

O que o reNut usa de terceiros. Tudo o que o build precisa está aqui ou nos
submódulos; o `prepare.py deps` não aplica patch de nada e só baixa o gerador de
código (última seção).

| Pasta | O que é | Como entra | Alterado? |
| --- | --- | --- | --- |
| `rexglue-sdk` | [RexGlue SDK](https://github.com/rexglue/rexglue-sdk), tag `v0.10.0` | submódulo | não |
| `plume` | [Plume](https://github.com/renderbag/plume), `d890ac8` | submódulo | não |
| `XenosRecomp` | conversor de shaders do Xbox 360 para HLSL/SPIR-V | cópia | **sim**: o patch do reNut já está aplicado |
| `libadrenotools` | carrega o driver Turnip empacotado no app (Android) | cópia | não |
| `libucontext` | fibras para o código recompilado (Android, ARM64) | cópia | não |

Os dois submódulos continuam intactos (`git submodule status` não mostra
diferença). As três cópias não têm `.git`: a origem, a revisão e o que foi
mudado estão abaixo e, para o `XenosRecomp`, em `XenosRecomp/RENUT.md`.

## XenosRecomp

- Origem: <https://github.com/masterspike52/reNut-XenosRecomp>, revisão
  `8f781f8e5f06e0ddd9c03c8155891af8f5c3786c`.
- Mudança: `XenosRecomp/renut-xenosrecomp.patch` (1.849 linhas: o estágio de
  domínio para a água, as constantes e os formatos do reNut). **Já está
  aplicado** nos fontes desta pasta; o arquivo fica só como registro e não deve
  ser aplicado de novo.
- O que foi tirado da cópia: o que o build não usa (testes e documentação de
  `fmt`, `smol-v`, `zstd`, `xxHash`) e o DXC de outras plataformas. A cópia só
  compila e roda em **Linux x64** (o `prepare.py` usa o WSL no Windows): em
  `thirdparty/dxc-bin` ficam `lib/x64/libdxcompiler.so`, `lib/x64/libdxil.so`,
  `bin/x64/dxc-linux` e os cabeçalhos.
- Compilação: `tools/shaders/recompile_shaders.sh`, que copia esta pasta para
  `~/.cache/renut-shader-tool/src` (no WSL) e compila ali.

## libadrenotools e libucontext

Cópias sem alteração, sem `.git` e sem os arquivos de CI:

- `libadrenotools`: <https://github.com/bylaws/libadrenotools>, `8fae8ce254df`,
  com o submódulo `lib/linkernsbypass` (<https://github.com/bylaws/liblinkernsbypass>)
  já dentro. Licença BSD-2-Clause (`LICENSE`).
- `libucontext`: <https://github.com/kaniini/libucontext>, `49e671dd52ff`.
  Licença ISC (`LICENSE`).

## O gerador de código (fora do repositório)

O `rexglue`, o gerador de código do SDK, **não está aqui**: o passo `deps` baixa
o pacote da sua plataforma de
<https://github.com/rexglue/rexglue-sdk/releases/tag/v0.10.0> para `external/`
(ignorado pelo Git), confere o sha256 (a tabela está em `tools/prepare.py`) e só
baixa uma vez.
