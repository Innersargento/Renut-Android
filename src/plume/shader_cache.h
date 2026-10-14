#pragma once

// The shaders XenosRecomp recompiled (generated/shaders, see
// tools/shaders/recompile_shaders.sh): one entry per shader container, keyed by
// the XXH3-64 of the container, with its SPIR-V as a slice of one smol-v blob.

#include <cstddef>
#include <cstdint>

#include "shader_common.h"

struct ShaderCacheEntry
{
    uint64_t hash;
    uint32_t dxilOffset;
    uint32_t dxilSize;
    uint32_t spirvOffset;
    uint32_t spirvSize;
    uint32_t specConstantsMask;

    // Slice of g_shaderInputElements: the vertex inputs the shader fetches,
    // for draws that arrive without a vertex declaration. Empty for pixel
    // shaders.
    uint32_t inputElementOffset;
    uint32_t inputElementCount;
};

extern ShaderCacheEntry g_shaderCacheEntries[];
extern const size_t g_shaderCacheEntryCount;

extern ShaderInputElement g_shaderInputElements[];
extern const size_t g_shaderInputElementCount;

// XenosRecomp's own zstd-compressed blob. Only tools/shaders/verify_cache.cpp
// sees it (recompile_shaders.sh strips it from the copy the app builds), but it
// must be declared here: the generated definitions are `const` and would
// otherwise have internal linkage.
extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
extern const size_t g_spirvCacheDecompressedSize;

// generated/shaders/shader_spirv.smolv, embedded by shader_cache_blob.S.
extern "C" const uint8_t g_spirvCache[];
extern "C" const uint8_t g_spirvCacheEnd[];
