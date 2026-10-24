# reNut para Android

Banjo-Kazooie: Nuts & Bolts no Android (ARM64), por **recompilação estática**: o
executável do Xbox 360 é traduzido para C++ e compilado para o aparelho, e o
Direct3D do jogo é desenhado por um renderizador próprio sobre Vulkan. Este
repositório é só o app Android.

Este projeto vem do [reNut](https://github.com/masterspike52/reNut) de
masterspike52 (a recompilação para PC) e usa o
[RexGlue SDK](https://github.com/rexglue/rexglue-sdk).

> **O repositório não traz nada do jogo.** Você precisa da sua própria cópia
> (a edição **europeia**: o app confere o SHA-256 do `default.xex`) e a importa no
> app. O código gerado e os shaders saem do seu jogo, ficam na sua máquina e nunca
> entram no Git.

## O que tem aqui

| Pasta | O quê |
|---|---|
| `android/` | o app (Gradle, Kotlin/Java) e o `native/CMakeLists.txt` da biblioteca nativa |
| `src/` | o código nativo: `guest/` (o jogo e o kernel emulado), `d3d/` (Direct3D do jogo), `plume/` (o renderizador), `platform/` (JNI, áudio, toque) |
| `config/` | a configuração do codegen: funções, ganchos e a CRT do jogo |
| `thirdparty/` | RexGlue SDK e Plume (submódulos, sem alteração) e as ferramentas de build (`thirdparty/README.md`) |
| `tools/` | `prepare.py` (o build de ponta a ponta), `shaders/`, `i18n/`, `bench/` (medição no aparelho), `test/` |
| `docs/` | como compilar, as dependências e as medições de desempenho (`docs/performance/`) |

## Como compilar

```bash
python tools/prepare.py doctor                 # confere a máquina
python tools/prepare.py all --game "<ISO ou pasta do jogo>"
python tools/prepare.py apk                    # ou abra android/ no Android Studio
```

O passo a passo, os pré-requisitos e os problemas conhecidos estão em
[docs/BUILD.md](docs/BUILD.md). As dependências e o que mudamos nelas estão em
[docs/DEPENDENCIAS.md](docs/DEPENDENCIAS.md).

## Medir desempenho

`tools/bench/` tem os scripts que medem no aparelho conectado por `adb`
(FPS, CPU das threads de GPU, A/B de uma opção). As medições feitas e o que
delas saiu estão em [docs/performance/](docs/performance/).

## Créditos

- **reNut** (masterspike52), a recompilação do jogo para PC de que este
  projeto parte, e quem a ajudou: SolarCookies (ganchos), ValcomDrifty (logo),
  OlieGamerTV (documentação do jogo), XephyrCraft e os testadores.
- **RexGlue SDK**, a base da recompilação.
- **Plume** (renderbag), a camada de renderização sobre Vulkan.
- **XenosRecomp** (hedge-dev), o conversor de shaders do Xbox 360, na versão do reNut.
- **libadrenotools** e **liblinkernsbypass** (bylaws), que carregam o driver
  Turnip empacotado; **libucontext** (kaniini), as fibras do código recompilado.
- As licenças das bibliotecas embarcadas no app estão em
  `android/app/src/main/assets/licenses/`.

## Licença

Este repositório ainda não declara uma licença para o código do reNut Android.
Defina uma antes de publicá-lo, respeitando as licenças dos projetos acima.
