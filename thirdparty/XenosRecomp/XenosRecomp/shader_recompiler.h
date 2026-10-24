#pragma once

#include "shader.h"
#include "shader_code.h"
#include "shader_common.h"

struct StringBuffer
{
    std::string out;

    template<class... Args>
    void print(fmt::format_string<Args...> fmt, Args&&... args)
    {
        fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
    }

    template<class... Args>
    void println(fmt::format_string<Args...> fmt, Args&&... args)
    {
        fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
        out += '\n';
    }
};

struct ShaderRecompiler : StringBuffer
{
    uint32_t indentation = 0;
    bool isPixelShader = false;
    // A vertex shader the guest runs as the domain stage of a tessellated
    // draw. The container says nothing about it, so the caller sets this from
    // the list of shaders seen on a patch draw. See recompile().
    bool isDomainShader = false;
    const uint8_t* constantTableData = nullptr;
    std::unordered_map<uint32_t, VertexElement> vertexElements;

    // The shader's own vertex input layout, gathered as the vfetches are
    // emitted - see ShaderInputElement. Only the fetches that correspond to a
    // declared vertex element land here; one indexed by anything other than the
    // vertex id is not an input at all and goes through vfetchStream instead.
    std::vector<ShaderInputElement> inputLayout;

    // The last vfetch_full seen. A vfetch_mini carries only its destination and
    // offset and takes the fetch constant, the stride and the format from the
    // full fetch before it, exactly as the hardware does - without this a mini
    // fetch exports stride 0 and format 0, which is no layout at all.
    VertexFetchInstruction lastFullFetch{};
    bool haveLastFullFetch = false;
    std::unordered_map<uint32_t, std::string> interpolators;
    std::unordered_map<uint32_t, const ConstantInfo*> float4Constants;
    std::unordered_map<uint32_t, const char*> boolConstants;
    std::unordered_map<uint32_t, const char*> samplers;
    std::unordered_map<uint32_t, uint32_t> ifEndLabels;
    // Structured if/else emission: instruction indices where a "} else {" is
    // emitted (then-branch close is part of the event, not ifEndLabels).
    std::unordered_set<uint32_t> elseLabels;
    uint32_t specConstantsMask = 0;
#ifdef RENUT_RECOMP
    // General purpose registers some instruction has written so far, in
    // emission order. A vfetch indexed by one of them is not indexed by the
    // vertex id (N&B divides the index to instance meshes: vertex =
    // floor(index / 32), instance = the remainder), so it cannot be a vertex
    // input and reads its stream directly.
    uint64_t writtenRegisters[2] = {};
    uint32_t liveFetchCount = 0;
    void markWritten(uint32_t reg) { if (reg < 128) writtenRegisters[reg / 64] |= 1ull << (reg % 64); }
    bool isWritten(uint32_t reg) const { return reg < 128 && (writtenRegisters[reg / 64] >> (reg % 64)) & 1; }
#endif

#ifdef RENUT_RECOMP
    // Constant registers the shader's own float4 definition table initialises.
    // These are emitted as "float4 cN = ..." literals and take priority over
    // the generic constant file, which is what the rest of the registers read.
    std::unordered_set<uint32_t> literalConstants;
    // Loop constant slots the shader's own int4 definition table initialises.
    std::unordered_set<uint32_t> definedLoopConstants;
    // Loop constant slots referenced by LoopStart/LoopEnd. Every one of them
    // needs an "int4 iN" in scope. Both looping shaders in the current dump
    // define the slots they use, but a shader is free to loop on a slot the
    // game sets through SetVertexShaderConstantI instead, and that one has to
    // come from the shared constant file rather than being left undeclared.
    std::unordered_set<uint32_t> loopConstants;
    // Render targets and depth the pixel shader header declares, so an export
    // to an undeclared one is caught instead of naming a missing parameter.
    uint32_t pixelShaderOutputs = 0;
    // Nesting depth while emitting structured loops, so each level gets its own
    // iteration counter instead of clobbering the shared aL.
    uint32_t loopDepth = 0;
    // Anything the emitter could not represent. main() turns a non-empty list
    // into a recompile failure rather than letting bad HLSL reach DXC.
    std::vector<std::string> errors;

    template<class... Args>
    void error(fmt::format_string<Args...> fmt, Args&&... args)
    {
        errors.push_back(fmt::vformat(fmt.get(), fmt::make_format_args(args...)));
    }
#endif

#ifdef UNLEASHED_RECOMP
    bool hasMtxProjection = false;
    bool hasMtxPrevInvViewProjection = false;
#endif

    void indent()
    {
        for (uint32_t i = 0; i < indentation; i++)
            out += '\t';
    }

    void printDstSwizzle(uint32_t dstSwizzle, bool operand);
    void printDstSwizzle01(uint32_t dstRegister, uint32_t dstSwizzle);

    void recompile(const VertexFetchInstruction& instr, uint32_t address);
    void recompile(const TextureFetchInstruction& instr, bool bicubic);
    void recompile(const AluInstruction& instr);

    void recompile(const uint8_t* shaderData, const std::string_view& include);
};
