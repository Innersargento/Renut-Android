#include "shader.h"
#include "shader_recompiler.h"
#include "dxc_compiler.h"

#include <mutex>
#include <vector>

static std::unique_ptr<uint8_t[]> readAllBytes(const char* filePath, size_t& fileSize)
{
    FILE* file = fopen(filePath, "rb");
    fseek(file, 0, SEEK_END);
    fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);
    auto data = std::make_unique<uint8_t[]>(fileSize);
    fread(data.get(), 1, fileSize, file);
    fclose(file);
    return data;
}

static void writeAllBytes(const char* filePath, const void* data, size_t dataSize)
{
    FILE* file = fopen(filePath, "wb");
    fwrite(data, 1, dataSize, file);
    fclose(file);
}

struct RecompiledShader
{
    uint8_t* data = nullptr;
    // Recompile this container as the domain stage of a tessellated draw
    // instead of as a vertex shader. See ShaderRecompiler::isDomainShader.
    bool isDomainShader = false;
    IDxcBlob* dxil = nullptr;
    std::vector<uint8_t> spirv;
    uint32_t specConstantsMask = 0;
    std::string sourceName;

    // The shader's own vertex input layout, for hosts the game never hands a
    // vertex declaration to. Empty for pixel shaders.
    std::vector<ShaderInputElement> inputLayout;
};

// Per-shader recompile failures, collected from the parallel loop and reported before exit.
struct ShaderFailure
{
    XXH64_hash_t hash;
    std::string reason;
};

static std::mutex g_failureMutex;
static std::vector<ShaderFailure> g_failures;

int main(int argc, char** argv)
{
#ifndef XENOS_RECOMP_INPUT
    if (argc < 4)
    {
        printf("Usage: XenosRecomp [input path] [output path] [shader common header file path] [optional: HLSL dump dir]");
        return 0;
    }
#endif

    const char* input =
#ifdef XENOS_RECOMP_INPUT 
        XENOS_RECOMP_INPUT
#else
        argv[1]
#endif
    ;

    const char* output =
#ifdef XENOS_RECOMP_OUTPUT 
        XENOS_RECOMP_OUTPUT
#else
        argv[2]
#endif
        ;
    
    const char* includeInput =
#ifdef XENOS_RECOMP_INCLUDE_INPUT
        XENOS_RECOMP_INCLUDE_INPUT
#else
        argv[3]
#endif
        ;

    std::string hlslDumpDir;
    if (argc > 4)
        hlslDumpDir = argv[4];
    else if (const char* env = std::getenv("XENOS_RECOMP_HLSL_DUMP"))
        hlslDumpDir = env;

    // Shaders the game also runs as a domain stage, one container hash in hex
    // per line. Nothing in a container says which stage it is used as, and the
    // guest decides at draw time, so the list comes from the renderer's draw
    // log - it names the vertex shader of every patch draw.
    std::set<XXH64_hash_t> domainShaderHashes;
    {
        const char* listPath = argc > 5 ? argv[5] : std::getenv("XENOS_RECOMP_DOMAIN_SHADERS");
        if (listPath != nullptr && listPath[0] != '\0')
        {
            std::ifstream list(listPath);
            if (!list)
            {
                fmt::println(stderr, "Cannot read the domain shader list at {}.", listPath);
                return 1;
            }
            std::string line;
            while (std::getline(list, line))
            {
                const size_t start = line.find_first_not_of(" \t\r");
                if (start == std::string::npos || line[start] == '#')
                    continue;
                domainShaderHashes.insert(std::stoull(line.substr(start), nullptr, 16));
            }
            fmt::println("Domain shaders listed: {}", domainShaderHashes.size());
        }
    }

    size_t includeSize = 0;
    auto includeData = readAllBytes(includeInput, includeSize);
    std::string_view include(reinterpret_cast<const char*>(includeData.get()), includeSize);

    if (std::filesystem::is_directory(input))
    {
        std::vector<std::unique_ptr<uint8_t[]>> files;
        std::map<XXH64_hash_t, RecompiledShader> shaders;

        for (auto& file : std::filesystem::recursive_directory_iterator(input,
            std::filesystem::directory_options::follow_directory_symlink))
        {
            if (std::filesystem::is_directory(file))
            {
                continue;
            }
            
            size_t fileSize = 0;
            auto fileData = readAllBytes(file.path().string().c_str(), fileSize);
            bool foundAny = false;
            int containerIndex = 0;

            for (size_t i = 0; fileSize > sizeof(ShaderContainer) && i < fileSize - sizeof(ShaderContainer) - 1;)
            {
                auto shaderContainer = reinterpret_cast<const ShaderContainer*>(fileData.get() + i);
                size_t dataSize = shaderContainer->virtualSize + shaderContainer->physicalSize;

                if ((shaderContainer->flags & 0xFFFFFF00) == 0x102A1100 &&
                    dataSize <= (fileSize - i) &&
                    shaderContainer->field1C == 0 &&
                    shaderContainer->field20 == 0)
                {
                    XXH64_hash_t hash = XXH3_64bits(shaderContainer, dataSize);
                    auto shader = shaders.try_emplace(hash);
                    if (shader.second)
                    {
                        shader.first->second.data = fileData.get() + i;
                        std::string stem = file.path().stem().string();
                        if (containerIndex > 0)
                            stem += fmt::format(".{}", containerIndex);
                        shader.first->second.sourceName = std::move(stem);
                        foundAny = true;
                    }

                    ++containerIndex;
                    i += dataSize;
                }
                else
                {
                    i += sizeof(uint32_t);
                }
            }

            if (foundAny)
                files.emplace_back(std::move(fileData));
        }

        // A container the game uses both ways is recompiled twice, the second
        // time as a domain shader, under the tagged hash.
        for (XXH64_hash_t hash : domainShaderHashes)
        {
            auto source = shaders.find(hash);
            if (source == shaders.end())
            {
                fmt::println(stderr, "Domain shader 0x{:016X} is not in the input.", hash);
                continue;
            }
            RecompiledShader variant = source->second;
            variant.isDomainShader = true;
            if (!variant.sourceName.empty())
                variant.sourceName += ".domain";
            shaders.emplace(hash ^ RENUT_DOMAIN_SHADER_HASH_TAG, std::move(variant));
        }

        if (!hlslDumpDir.empty())
            std::filesystem::create_directories(hlslDumpDir);

        std::atomic<uint32_t> progress = 0;

        std::for_each(std::execution::par_unseq, shaders.begin(), shaders.end(), [&](auto& hashShaderPair)
            {
                auto& shader = hashShaderPair.second;
                const XXH64_hash_t hash = hashShaderPair.first;

                auto recordFailure = [hash](const char* reason)
                {
                    std::lock_guard<std::mutex> lock(g_failureMutex);
                    g_failures.push_back({hash, reason});
                };

                thread_local ShaderRecompiler recompiler;
                recompiler = {};
                recompiler.isDomainShader = shader.isDomainShader;
                recompiler.recompile(shader.data, include);

                shader.specConstantsMask = recompiler.specConstantsMask;
                shader.inputLayout = recompiler.inputLayout;

                if (!hlslDumpDir.empty())
                {
                    auto path = std::filesystem::path(hlslDumpDir) /
                        (shader.sourceName.empty()
                            ? fmt::format("{}_{:016X}",
                                recompiler.isPixelShader ? "ps" : (shader.isDomainShader ? "ds" : "vs"),
                                hash)
                            : shader.sourceName);
                    path += ".hlsl";

                    std::string contents = fmt::format(
                        "// {} shader  hash=0x{:016X}  specConstants=0x{:X}\n",
                        recompiler.isPixelShader ? "pixel" : (shader.isDomainShader ? "domain" : "vertex"),
                        hash, recompiler.specConstantsMask);
                    contents.append(recompiler.out);
                    writeAllBytes(path.string().c_str(), contents.data(), contents.size());
                }

#ifdef RENUT_RECOMP
                // Anything the emitter could not represent - an unmapped vertex
                // semantic, an export with nowhere to go - would otherwise
                // reach DXC as HLSL that references undeclared names, and the
                // diagnostic would be about the symptom rather than the cause.
                if (!recompiler.errors.empty())
                {
                    std::string reason = "recompiler:";
                    for (const auto& message : recompiler.errors)
                    {
                        reason += ' ';
                        reason += message;
                        reason += ';';
                    }
                    recordFailure(reason.c_str());
                    return;
                }
#endif

                thread_local DxcCompiler dxcCompiler;

#ifdef XENOS_RECOMP_DXIL
                shader.dxil = dxcCompiler.compile(recompiler.out, recompiler.isPixelShader, recompiler.specConstantsMask != 0, false);
                if (shader.dxil == nullptr)
                {
                    recordFailure("dxc-dxil-compile-failed");
                    return;
                }
                if (*(reinterpret_cast<uint32_t*>(shader.dxil->GetBufferPointer()) + 1) == 0)
                {
                    recordFailure("dxil-not-signed");
                    return;
                }
#endif

                IDxcBlob* spirv = dxcCompiler.compile(recompiler.out, recompiler.isPixelShader, false, true);
                if (spirv == nullptr)
                {
                    recordFailure("dxc-spirv-compile-failed");
                    return;
                }

                if (!smolv::Encode(spirv->GetBufferPointer(), spirv->GetBufferSize(), shader.spirv, smolv::kEncodeFlagStripDebugInfo))
                {
                    spirv->Release();
                    recordFailure("smolv-encode-failed");
                    return;
                }

                spirv->Release();

                size_t currentProgress = ++progress;
                if ((currentProgress % 10) == 0 || (currentProgress == shaders.size() - 1))
                    fmt::println("Recompiling shaders... {}%", currentProgress / float(shaders.size()) * 100.0f);
            });

        if (!g_failures.empty())
        {
            fmt::println(stderr, "Recompile failures ({}):", g_failures.size());
            for (const auto& failure : g_failures)
                fmt::println(stderr, "  hash=0x{:016X} reason={}", failure.hash, failure.reason);

            // Opt-in: write the cache without the shaders that failed, for
            // callers that have already accounted for every failure.
            const char* allow = std::getenv("XENOS_RECOMP_ALLOW_FAILURES");
            if (allow == nullptr || allow[0] != '1')
                return 2;

            for (const auto& failure : g_failures)
                shaders.erase(failure.hash);

            fmt::println(stderr, "Leaving {} failed shaders out of the cache.", g_failures.size());
        }

        fmt::println("Creating shader cache...");

        StringBuffer f;
        // Beside the generated .cpp, which is where renut keeps the
        // hand-written shader_cache.h - it is not generated, and the include is
        // load-bearing: the cache sizes are const size_t and would otherwise
        // take internal linkage and be invisible to the rest of the host.
        f.println("#include \"shader_cache.h\"");
        f.println("ShaderCacheEntry g_shaderCacheEntries[] = {{");

        std::vector<uint8_t> dxil;
        std::vector<uint8_t> spirv;

        // Every shader's input layout back to back, named by an offset and a
        // count in each entry, the same way the bytecode slices are.
        std::vector<ShaderInputElement> inputElements;

        for (auto& [hash, shader] : shaders)
        {
            f.println("\t{{ 0x{:X}, {}, {}, {}, {}, {}, {}, {} }},",
                hash, dxil.size(), (shader.dxil != nullptr) ? shader.dxil->GetBufferSize() : 0,
                spirv.size(), shader.spirv.size(), shader.specConstantsMask,
                inputElements.size(), shader.inputLayout.size());

            if (shader.dxil != nullptr)
            {
                dxil.insert(dxil.end(), reinterpret_cast<uint8_t *>(shader.dxil->GetBufferPointer()),
                    reinterpret_cast<uint8_t *>(shader.dxil->GetBufferPointer()) + shader.dxil->GetBufferSize());
            }

            spirv.insert(spirv.end(), shader.spirv.begin(), shader.spirv.end());

            inputElements.insert(inputElements.end(), shader.inputLayout.begin(),
                shader.inputLayout.end());
        }

        f.println("}};");

        f.println("ShaderInputElement g_shaderInputElements[] = {{");

        for (const ShaderInputElement& element : inputElements)
        {
            f.println("\t{{ {}, {}, {}, {}, {}, {}, {}, {}, {}, 0, {}, {} }},",
                element.usage, element.usageIndex, element.location, element.slot,
                element.offsetBytes, element.strideBytes, element.format, element.isSigned,
                element.isNormalized, element.fetchAddress, element.dstSwizzle);
        }

        f.println("}};");
        f.println("const size_t g_shaderInputElementCount = {};", inputElements.size());

        fmt::println("Compressing DXIL cache...");

        int level = ZSTD_maxCLevel();

#ifdef XENOS_RECOMP_DXIL
        std::vector<uint8_t> dxilCompressed(ZSTD_compressBound(dxil.size()));
        dxilCompressed.resize(ZSTD_compress(dxilCompressed.data(), dxilCompressed.size(), dxil.data(), dxil.size(), level));

        f.print("const uint8_t g_compressedDxilCache[] = {{");

        for (auto data : dxilCompressed)
            f.print("{},", data);

        f.println("}};");
        f.println("const size_t g_dxilCacheCompressedSize = {};", dxilCompressed.size());
        f.println("const size_t g_dxilCacheDecompressedSize = {};", dxil.size());
#endif

        fmt::println("Compressing SPIRV cache...");

        std::vector<uint8_t> spirvCompressed(ZSTD_compressBound(spirv.size()));
        spirvCompressed.resize(ZSTD_compress(spirvCompressed.data(), spirvCompressed.size(), spirv.data(), spirv.size(), level));

        f.print("const uint8_t g_compressedSpirvCache[] = {{");

        for (auto data : spirvCompressed)
            f.print("{},", data);

        f.println("}};");

        f.println("const size_t g_spirvCacheCompressedSize = {};", spirvCompressed.size());
        f.println("const size_t g_spirvCacheDecompressedSize = {};", spirv.size());
        f.println("const size_t g_shaderCacheEntryCount = {};", shaders.size());

        writeAllBytes(output, f.out.data(), f.out.size());
    }
    else
    {
        ShaderRecompiler recompiler;
        size_t fileSize;
        recompiler.recompile(readAllBytes(input, fileSize).get(), include);
        writeAllBytes(output, recompiler.out.data(), recompiler.out.size());
    }

    return 0;
}
