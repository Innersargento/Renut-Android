# reNut for Android

Banjo-Kazooie: Nuts & Bolts on Android (ARM64), through **static recompilation**:
the Xbox 360 executable is translated to C++ and compiled for the device, and the
game's Direct3D is drawn by a renderer of our own on top of Vulkan. This
repository is only the Android app.

This project comes from [reNut](https://github.com/masterspike52/reNut) by
masterspike52 (the recompilation for PC) and uses the
[RexGlue SDK](https://github.com/rexglue/rexglue-sdk).

> **Special thanks to [masterspike52](https://github.com/masterspike52) and the
> [reNut](https://github.com/masterspike52/reNut) project.** Without their
> recompilation of the game, its hooks and its documentation, none of this would
> exist.

> **The repository contains nothing of the game.** You need your own copy (the
> **European** edition: the app checks the SHA-256 of `default.xex`) and import it
> in the app. The generated code and the shaders come out of your game, stay on
> your machine and never enter Git.

## What is here

| Folder | What |
|---|---|
| `android/` | the app (Gradle, Kotlin/Java) and the native library's `native/CMakeLists.txt` |
| `src/` | the native code: `guest/` (the game and the emulated kernel), `d3d/` (the game's Direct3D), `plume/` (the renderer), `platform/` (JNI, audio, touch) |
| `config/` | the codegen configuration: functions, hooks and the game's CRT |
| `thirdparty/` | RexGlue SDK and Plume (submodules, unmodified) and the build tools (`thirdparty/README.md`) |
| `tools/` | `prepare.py` (the end-to-end build), `shaders/`, `i18n/`, `bench/` (measuring on the device), `test/` |
| `docs/` | how to build, the dependencies and the performance measurements (`docs/performance/`) |

## How to build

```bash
python tools/prepare.py doctor                 # checks the machine
python tools/prepare.py all --game "<ISO or game folder>"
python tools/prepare.py apk                    # or open android/ in Android Studio
```

The step by step, the prerequisites and the known problems are in
[docs/BUILD.md](docs/BUILD.md). The dependencies and what we changed in them are
in [docs/DEPENDENCIAS.md](docs/DEPENDENCIAS.md). (These two documents are in
Portuguese.)

## Measuring performance

`tools/bench/` has the scripts that measure on a device connected through `adb`
(FPS, CPU of the GPU threads, A/B of an option). The measurements made and what
came out of them are in [docs/performance/](docs/performance/).

## Credits

- **[reNut](https://github.com/masterspike52/reNut)** (masterspike52), the
  recompilation of the game for PC this project starts from, and those who helped
  it: SolarCookies (hooks), ValcomDrifty (logo), OlieGamerTV (game
  documentation), XephyrCraft and the testers.
- **RexGlue SDK**, the base of the recompilation.
- **Plume** (renderbag), the rendering layer on top of Vulkan.
- **XenosRecomp** (hedge-dev), the Xbox 360 shader converter, in reNut's version.
- **libadrenotools** and **liblinkernsbypass** (bylaws), which load the bundled
  Turnip driver; **libucontext** (kaniini), the fibers of the recompiled code.
- The licenses of the libraries embedded in the app are in
  `android/app/src/main/assets/licenses/`.

## License

This repository does not declare a license yet for the reNut Android code. Set
one before publishing it, respecting the licenses of the projects above.
