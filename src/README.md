# src/

The code reNut itself writes. The SDK (`thirdparty/rexglue-sdk`) is used as released.

```
d3d/            the game's Direct3D, taken over from the recompiled code
  hooks/        functions.h: the one table of intercepted functions (X-macro); layer.*
                makes a REX_HOOK_RAW of each; handlers.cpp registers the native handlers
  device/       the device's shadow state: render/sampler state, bound objects, getters,
                small leaf functions, register emitters and state resets (lists.cpp); device.h has the layout
  resources/    vertex/index buffers, textures, surfaces; shaders and their microcode
  scene/        frame-level calls: swap, tiling (EDRAM bands), DrawIndexedVerticesUP, export
  sync/         fences
  frontend/     the native front end: draws, clears and resolves recorded as host lists
gpu/            the GPU backend that runs those lists on Plume (renut::gpu)
  core/         the backend object, options, context, shared types; draw_state.h is the state a draw reads
                (the front end's records write it by word, there is no register file), state_bits.h the bit
                layouts of its words
  exec/         executes the lists (native_exec.cpp), records Vulkan commands
  draw/         draws, streams, pipelines and their cache, shaders, patches, Hi-Z
  targets/      render targets, resolves, textures, the swap
  debug/        format census
  xenos/        microcode analysis, interpreter, translator
plume/          the Plume renderer glue: swapchain, textures, frame pacing, host shaders
guest/          what is done to the recompiled game: kernel overrides, fixes, cheats
platform/       Android: JNI, audio, ADPF, users, text input, Vulkan loader
vendor/         smolv
```

Include project headers by their path from `src/` (`#include "gpu/core/backend.h"`).
