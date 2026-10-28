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
> in the app.

## How to build

```bash
python tools/prepare.py doctor                 # checks the machine
python tools/prepare.py all --game "<ISO or game folder>"
python tools/prepare.py apk                    # or open android/ in Android Studio
```


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

