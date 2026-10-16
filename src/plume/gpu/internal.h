#pragma once
// The Plume GPU backend, split by concern across src/plume/gpu/.
//
// Headers, each including the one before it:
//   base.h            includes, constants and small helpers
//   shader_store.h    HostShader and the store of recompiled shaders
//   recipes.h         pipeline recipes, precompilation and prediction
//   formats.h         Xenos-to-Vulkan format and state translation
//   resources.h       render targets, resolved images, guest textures, statistics
//   backend.h         the GpuBackend class
//
// Sources:
//   options.cpp       the renut_plume_* cvars
//   support.cpp       free helpers (formats, blend, shader inputs, recipes, the ucode map)
//   pipeline_cache.cpp  pipeline workers, recipes, pipeline prediction
//   context.cpp       Vulkan context set-up and tear-down, the worker loop
//   streams.cpp       upload ring, kept vertex streams, memory exports, constants
//   profiling.cpp     GPU timestamps and the pair profiler
//   targets.cpp       frame start, render targets, framebuffers, resolved images
//   textures.cpp      guest textures and samplers
//   shaders.cpp       register writes, shader lookup, pipeline statistics
//   native_exec.cpp   the native lists the D3D front end records
//   draw.cpp          IssueDraw
//   recorder.cpp      draw recording thread, deferred draws, device-lost report
//   hiz.cpp           Hi-Z culling
//   resolve.cpp       resolves and depth copies
//   patches.cpp       tessellation patch expansion
//   swap.cpp          IssueSwap, the frame's present
//   report.cpp        memory and statistics reports
//   host.cpp          PlumeGpu (the hosted video hardware) and the public API of plume_gpu.h
//
// All of it lives in renut::gpu. Nothing here is for other parts of the app; they use
// plume_gpu.h.
#include "backend.h"
