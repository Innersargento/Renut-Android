// Checks a generated shader_cache.cpp: every entry decodes to SPIR-V, and the
// SPIR-V is written out so spirv-val can check it.
//
//   clang++ -std=c++17 -I<dir with shader_cache.h and shader_common.h> \
//       -I<smol-v/source> verify_cache.cpp shader_cache.cpp smolv.cpp -lzstd
//   ./a.out <spirv out dir> [smol-v blob out]

#include "shader_cache.h"

#include <smolv.h>
#include <zstd.h>

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: verify_cache <spirv out dir>\n");
        return 1;
    }

    std::vector<uint8_t> smolv(g_spirvCacheDecompressedSize);
    size_t size = ZSTD_decompress(smolv.data(), smolv.size(), g_compressedSpirvCache,
        g_spirvCacheCompressedSize);
    if (ZSTD_isError(size) || size != smolv.size())
    {
        std::fprintf(stderr, "zstd: %s\n", ZSTD_isError(size) ? ZSTD_getErrorName(size) : "size mismatch");
        return 1;
    }

    size_t failures = 0, spirvBytes = 0, largest = 0, vertexInputs = 0;
    for (size_t i = 0; i < g_shaderCacheEntryCount; i++)
    {
        const ShaderCacheEntry& entry = g_shaderCacheEntries[i];
        // Every manual fetch occupies its own host table entry.
        uint32_t liveSlots = 0;
        bool terrainDetail = false, terrainWorld = false;
        for (uint32_t j = 0; j < entry.inputElementCount; ++j)
        {
            const auto& element = g_shaderInputElements[entry.inputElementOffset + j];
            if (element.location == RENUT_LIVE_FETCH_LOCATION)
            {
                if (element.slot >= RENUT_LIVE_FETCH_COUNT ||
                    (liveSlots & (1u << element.slot)))
                {
                    std::fprintf(stderr, "%016llX: invalid or aliased live fetch slot\n",
                        (unsigned long long)entry.hash);
                    failures++;
                }
                else liveSlots |= 1u << element.slot;
            }
            terrainDetail |= element.fetchAddress == 9 && element.location == 8;
            terrainWorld |= element.fetchAddress == 11 &&
                element.location == RENUT_LIVE_FETCH_LOCATION;
        }
        // Captured terrain regression: two TEXCOORD0 instructions read byte
        // offsets 20 and 24. Collapsing them repeats the world mask as rosettes.
        if (entry.hash == 0x239A125831F7EB9Dull && (!terrainDetail || !terrainWorld))
        {
            std::fprintf(stderr, "terrain detail/world UV fetches were merged\n");
            failures++;
        }
        const uint8_t* data = smolv.data() + entry.spirvOffset;

        size_t decodedSize = smolv::GetDecodedBufferSize(data, entry.spirvSize);
        std::vector<uint8_t> spirv(decodedSize);
        if (decodedSize == 0 || !smolv::Decode(data, entry.spirvSize, spirv.data(), decodedSize))
        {
            std::fprintf(stderr, "%016llX: smol-v decode failed\n", (unsigned long long)entry.hash);
            failures++;
            continue;
        }

        spirvBytes += decodedSize;
        largest = decodedSize > largest ? decodedSize : largest;
        vertexInputs += entry.inputElementCount;

        char path[512];
        std::snprintf(path, sizeof(path), "%s/%016llX.spv", argv[1], (unsigned long long)entry.hash);
        FILE* file = std::fopen(path, "wb");
        std::fwrite(spirv.data(), 1, spirv.size(), file);
        std::fclose(file);
    }

    // Optionally keep the decompressed smol-v blob: the Android build embeds it
    // as is, so the runtime needs smol-v but not zstd.
    if (argc > 2)
    {
        FILE* blob = std::fopen(argv[2], "wb");
        std::fwrite(smolv.data(), 1, smolv.size(), blob);
        std::fclose(blob);
    }

    std::printf("entries %zu, decode failures %zu, SPIR-V %zu bytes (avg %zu, max %zu), input elements %zu/%zu\n",
        g_shaderCacheEntryCount, failures, spirvBytes, spirvBytes / (g_shaderCacheEntryCount ? g_shaderCacheEntryCount : 1),
        largest, vertexInputs, g_shaderInputElementCount);
    return failures == 0 ? 0 : 1;
}
