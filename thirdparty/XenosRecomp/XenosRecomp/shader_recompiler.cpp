#include "shader_recompiler.h"
#include "shader_common.h"

static constexpr char SWIZZLES[] =
{
    'x',
    'y',
    'z',
    'w',
    '0',
    '1',
    '_',
    '_'
};

static constexpr const char* USAGE_TYPES[] =
{
    "float4", // POSITION
    "float4", // BLENDWEIGHT
#ifdef RENUT_RECOMP
    // The hardware hands every vfetch to the shader as floats, and N&B stores
    // bone indices as UBYTE4N (scaled by 255 in the shader) and FLOAT16_2 as
    // well as UBYTE4, so the input is the float the guest shader expected.
    "float4", // BLENDINDICES
#else
    "uint4", // BLENDINDICES
#endif
#ifdef RENUT_RECOMP
    // BD IA-decodes SNORM normals to float; swapFloats() undoes the engine int16-pair swap, DEC3N goes through tfetchR11G11B10().
    "float4", // NORMAL
#else
    "uint4", // NORMAL
#endif
    "float4", // PSIZE
    "float4", // TEXCOORD
#ifdef RENUT_RECOMP
    "float4", // TANGENT
    "float4", // BINORMAL
#else
    "uint4", // TANGENT
    "uint4", // BINORMAL
#endif
    "float4", // TESSFACTOR
    "float4", // POSITIONT
    "float4", // COLOR
    "float4", // FOG
    "float4", // DEPTH
    "float4", // SAMPLE
};

static constexpr const char* USAGE_VARIABLES[] =
{
    "Position",
    "BlendWeight",
    "BlendIndices",
    "Normal",
    "PointSize",
    "TexCoord",
    "Tangent",
    "Binormal",
    "TessFactor",
    "PositionT",
    "Color",
    "Fog",
    "Depth",
    "Sample"
};

static constexpr const char* USAGE_SEMANTICS[] =
{
    "POSITION",
    "BLENDWEIGHT",
    "BLENDINDICES",
    "NORMAL",
    "PSIZE",
    "TEXCOORD",
    "TANGENT",
    "BINORMAL",
    "TESSFACTOR",
    "POSITIONT",
    "COLOR",
    "FOG",
    "DEPTH",
    "SAMPLE"
};

static bool isStreamFetchFormatSupported(uint32_t format)
{
    switch (format)
    {
    case 6: case 7: case 16: case 17: case 25: case 26: case 31: case 32:
    case 33: case 34: case 35: case 36: case 37: case 38: case 57:
        return true;
    default:
        return false;
    }
}

struct DeclUsageLocation
{
    DeclUsage usage;
    uint32_t usageIndex;
    uint32_t location;
};

static constexpr DeclUsageLocation USAGE_LOCATIONS[] =
{
#ifdef RENUT_RECOMP
    #define RENUT_VERTEX_LOCATION_ROW(usage, index, location) { DeclUsage::usage, index, location },
    RENUT_VERTEX_INPUT_LOCATIONS(RENUT_VERTEX_LOCATION_ROW)
    #undef RENUT_VERTEX_LOCATION_ROW
#else
    { DeclUsage::Position, 0, 0 },
    { DeclUsage::Normal, 0, 1 },
    { DeclUsage::Tangent, 0, 2 },
    { DeclUsage::Binormal, 0, 3 },
    { DeclUsage::TexCoord, 0, 4 },
    { DeclUsage::TexCoord, 1, 5 },
    { DeclUsage::TexCoord, 2, 6 },
    { DeclUsage::TexCoord, 3, 7 },
    { DeclUsage::Color, 0, 8 },
    { DeclUsage::BlendIndices, 0, 9 },
    { DeclUsage::BlendWeight, 0, 10 },
    { DeclUsage::Color, 1, 11 },
    { DeclUsage::TexCoord, 4, 12 },
    { DeclUsage::TexCoord, 5, 13 },
    { DeclUsage::TexCoord, 6, 14 },
    { DeclUsage::TexCoord, 7, 15 },
    { DeclUsage::Position, 1, 15 },
#endif
};

static constexpr std::pair<DeclUsage, size_t> INTERPOLATORS[] =
{
#ifdef RENUT_RECOMP
    // Shared with the host so a vertex and a pixel stage always agree on the
    // varying set. N&B interpolates NORMAL/TANGENT/BINORMAL directly, which
    // the TEXCOORD/COLOR-only list this was forked from could not express.
    #define RENUT_INTERPOLATOR_ROW(usage, index) { DeclUsage::usage, index },
    RENUT_INTERPOLATORS(RENUT_INTERPOLATOR_ROW)
    #undef RENUT_INTERPOLATOR_ROW
#else
    { DeclUsage::TexCoord, 0 },
    { DeclUsage::TexCoord, 1 },
    { DeclUsage::TexCoord, 2 },
    { DeclUsage::TexCoord, 3 },
    { DeclUsage::TexCoord, 4 },
    { DeclUsage::TexCoord, 5 },
    { DeclUsage::TexCoord, 6 },
    { DeclUsage::TexCoord, 7 },
    { DeclUsage::TexCoord, 8 },
    { DeclUsage::TexCoord, 9 },
    { DeclUsage::TexCoord, 10 },
    { DeclUsage::TexCoord, 11 },
    { DeclUsage::TexCoord, 12 },
    { DeclUsage::TexCoord, 13 },
    { DeclUsage::TexCoord, 14 },
    { DeclUsage::TexCoord, 15 },
    { DeclUsage::Color, 0 },
    { DeclUsage::Color, 1 }
#endif
};

#ifdef RENUT_RECOMP
// Xenos has 64 general purpose registers and N&B reaches r63; the 32 this was
// forked with left every reference above r31 undeclared.
static constexpr uint32_t GPR_COUNT = 64;
#else
static constexpr uint32_t GPR_COUNT = 32;
#endif

static FetchDestinationSwizzle getDestSwizzle(uint32_t dstSwizzle, uint32_t index)
{
    return FetchDestinationSwizzle((dstSwizzle >> (index * 3)) & 0x7);
}

void ShaderRecompiler::printDstSwizzle(uint32_t dstSwizzle, bool operand)
{
    for (size_t i = 0; i < 4; i++)
    {
        const auto swizzle = getDestSwizzle(dstSwizzle, i);
        if (swizzle >= FetchDestinationSwizzle::X && swizzle <= FetchDestinationSwizzle::W)
            out += SWIZZLES[operand ? uint32_t(swizzle) : i];
    }
}

void ShaderRecompiler::printDstSwizzle01(uint32_t dstRegister, uint32_t dstSwizzle)
{
    for (size_t i = 0; i < 4; i++)
    {
        const auto swizzle = getDestSwizzle(dstSwizzle, i);
        if (swizzle == FetchDestinationSwizzle::Zero)
        {
            indent();
            println("r{}.{} = 0.0;", dstRegister, SWIZZLES[i]);
        }
        else if (swizzle == FetchDestinationSwizzle::One)
        {
            indent();
            println("r{}.{} = 1.0;", dstRegister, SWIZZLES[i]);
        }
    }
}

void ShaderRecompiler::recompile(const VertexFetchInstruction& instr, uint32_t address)
{
    if (instr.isPredicated)
    {
        indent();
        println("if ({}p0)", instr.predicateCondition ? "" : "!");

        indent();
        out += "{\n";
        ++indentation;
    }

    indent();
#ifdef RENUT_RECOMP
    // Whether the index register was written before this fetch, read ahead of
    // the fetch marking its own destination: r0 = vfetch(r0.x), the commonest
    // fetch there is, is indexed by the vertex id and not by itself. Marking
    // first sent 800 of the 874 vertex shaders down vfetchLive - a raw buffer
    // load and a switch over every format for each input, the GPU fetching
    // what its input assembler would have.
    const bool indexWritten = isWritten(uint32_t(instr.srcRegister));
    markWritten(uint32_t(instr.dstRegister));
#endif
    print("r{}.", instr.dstRegister);
    printDstSwizzle(instr.dstSwizzle, false);

    out += " = ";

    auto findResult = vertexElements.find(address);

#ifdef RENUT_RECOMP
    // A vfetch the vertex declaration does not describe.
    //
    // Not every vfetch is a vertex input. The hardware indexes a stream with
    // whatever register the instruction names, and only when that register is
    // the vertex id does the fetch correspond to an input attribute - which is
    // the only case D3D's vertex declaration can express, and therefore the
    // only case that appears in this shader's element table.
    //
    // N&B's skinned meshes are the other case: their vertex shaders take
    // POSITION, BLENDINDICES and BLENDWEIGHT as inputs and then fetch bone
    // matrices out of a stream by blend index, three consecutive float4 rows at
    // a 64-byte stride, accumulating them by weight. 98 of the 1169 shaders in
    // the dump do this, all from stream 10.
    //
    // The stream is recoverable: D3DDevice_SetStreamSource (0x8222A408) writes
    // vertex fetch constant 95 - StreamNumber, and the instruction names fetch
    // constant constIndex * 3 + constIndexSelect. So the host binds the stream
    // and this reads it as a raw buffer.
    // A declared input indexed by a register the shader computed is not
    // indexed by the vertex id, so the host input assembler cannot fetch it.
    const bool indexedByComputedRegister =
        findResult != vertexElements.end() && indexWritten;
    // A declaration semantic is not a unique fetch. Terrain shaders read
    // TEXCOORD0 twice, at offsets 20 (detail UV) and 24 (world-map UV).
    // Preserve subsequent instructions independently, including D3D's live
    // stream/format/offset patching, instead of aliasing the first attribute.
    const bool repeatedSemantic = findResult != vertexElements.end() &&
        std::any_of(inputLayout.begin(), inputLayout.end(),
            [&](const ShaderInputElement& element)
            {
                return element.usage == uint8_t(findResult->second.usage) &&
                    element.usageIndex == uint8_t(findResult->second.usageIndex);
            });
    if (indexedByComputedRegister || repeatedSemantic)
    {
        if (!instr.isMiniFetch)
        {
            lastFullFetch = instr;
            haveLastFullFetch = true;
        }

        // D3D rewrites the vfetch at bind time for the vertex buffer actually
        // bound, so the stream, format, stride and offset in the microcode as
        // registered are not the ones in use. The host decodes the live
        // instruction at fetchAddress and fills slot k of the fetch table.
        const uint32_t k = liveFetchCount++;
        ShaderInputElement element{};
        element.usage = uint8_t(findResult->second.usage);
        element.usageIndex = uint8_t(findResult->second.usageIndex);
        element.location = RENUT_LIVE_FETCH_LOCATION;
        element.slot = uint8_t(k);
        element.fetchAddress = uint16_t(address);
        element.dstSwizzle = uint16_t(instr.dstSwizzle);
        inputLayout.push_back(element);

        indent();
        print("r{}.", instr.dstRegister);
        printDstSwizzle(instr.dstSwizzle, false);
        print(" = vfetchLive({}, r{}.{}).", k, uint32_t(instr.srcRegister),
            SWIZZLES[uint32_t(instr.srcSwizzle) & 3]);
        printDstSwizzle(instr.dstSwizzle, true);
        out += ";\n";
        printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);

        if (instr.isPredicated)
        {
            --indentation;
            indent();
            out += "}\n";
        }
        return;
    }

    if (findResult == vertexElements.end() || indexedByComputedRegister)
    {
        if (!instr.isMiniFetch)
        {
            lastFullFetch = instr;
            haveLastFullFetch = true;
        }

        const VertexFetchInstruction& fetchSource =
            (instr.isMiniFetch && haveLastFullFetch) ? lastFullFetch : instr;

        const uint32_t fetchConstant =
            uint32_t(fetchSource.constIndex) * 3 + uint32_t(fetchSource.constIndexSelect);

        // Streams 0..15 are what SetStreamSource binds, vertex fetch constants
        // 95 - StreamNumber. Direct3D's own shaders (the rectangle-list Clear)
        // fetch through constant 0, which gets the extra entry. Anything else
        // is not a stream source, and guessing would bind the wrong buffer.
        const int32_t stream = fetchConstant == 0 ? RENUT_DIRECT_FETCH_STREAM
            : (fetchConstant <= 95 && (95 - fetchConstant) < 16) ? int32_t(95 - fetchConstant) : -1;
        if (stream < 0)
        {
            error("vfetch from address {} names fetch constant {}, which is not a stream source",
                address, fetchConstant);
            out += "0.0;\n";
        }
        else if (!isStreamFetchFormatSupported(uint32_t(instr.format)))
        {
            // Reading an unknown format as anything would be silently wrong
            // rather than missing.
            error("vfetch from address {} on stream {} has unsupported format {}",
                address, stream, uint32_t(instr.format));
            out += "0.0;\n";
        }
        else if (instr.format != 38 || instr.expAdjust != 0 ||
            (uint32_t(fetchSource.stride) % 4) != 0 || (uint32_t(instr.offset) % 4) != 0)
        {
            // (The float4 fast path below loads with 16-byte alignment, so a
            // stride or offset that is not a multiple of four dwords - the
            // Clear's 28-byte position + colour vertices - takes this one.)
            // Skinning's bone rows (k_32_32_32_32_FLOAT) keep the aligned fast
            // path below; everything else - N&B's particle and sprite shaders
            // pull k_8_8_8_8 colours and k_16_16 corners the same way - goes
            // through the generic unpack.
            const uint32_t flags = uint32_t(instr.formatCompAll) |
                (uint32_t(instr.numFormatAll) << 1) | (uint32_t(instr.signedRfModeAll) << 2);

            print("vfetchStreamFormat({}, r{}.{}, {}, {}, {}, {}, {})", stream,
                uint32_t(instr.srcRegister), SWIZZLES[uint32_t(instr.srcSwizzle) & 3],
                uint32_t(fetchSource.stride) * 4, uint32_t(instr.offset) * 4,
                uint32_t(instr.format), flags, std::ldexp(1.0f, int32_t(instr.expAdjust)));

            out += '.';
            printDstSwizzle(instr.dstSwizzle, true);
            out += ";\n";

            printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);
        }
        else
        {
            // stride and offset are in dwords, which is what makes a 16-stride
            // fetch at offsets 0, 4 and 8 three whole float4s rather than three
            // overlapping ones.
            print("vfetchStream({}, r{}.{}, {}, {})", stream,
                uint32_t(instr.srcRegister), SWIZZLES[uint32_t(instr.srcSwizzle) & 3],
                uint32_t(fetchSource.stride) * 4, uint32_t(instr.offset) * 4);

            out += '.';
            printDstSwizzle(instr.dstSwizzle, true);
            out += ";\n";

            printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);
        }

        if (instr.isPredicated)
        {
            --indentation;
            indent();
            out += "}\n";
        }
        return;
    }
#endif

    assert(findResult != vertexElements.end());

#ifdef RENUT_RECOMP
    // Wrap each 16-bit-packed semantic in swapFloats() (per-usage mask); TEXCOORD also runs sintTexcoord() for raw-int bindings.
    switch (findResult->second.usage)
    {
    case DeclUsage::Normal:
        specConstantsMask |= SPEC_CONSTANT_R11G11B10_NORMAL;
        print("tfetchR11G11B10(swapFloats(g_SwappedNormals, ");
        break;
    case DeclUsage::Tangent:
        specConstantsMask |= SPEC_CONSTANT_R11G11B10_NORMAL;
        print("tfetchR11G11B10(swapFloats(g_SwappedTangents, ");
        break;
    case DeclUsage::Binormal:
        specConstantsMask |= SPEC_CONSTANT_R11G11B10_NORMAL;
        print("tfetchR11G11B10(swapFloats(g_SwappedBinormals, ");
        break;
    case DeclUsage::BlendWeight:
        print("swapFloats(g_SwappedBlendWeights, ");
        break;
    case DeclUsage::BlendIndices:
        print("swapFloats(g_SwappedBlendIndices, ");
        break;
    case DeclUsage::TexCoord:
        print("sintTexcoord(g_SintTexcoords, swapFloats(g_SwappedTexcoords, ");
        break;
    case DeclUsage::Position:
        print("swapFloats(g_SwappedPositions, ");
        break;
    }
#else
    switch (findResult->second.usage)
    {
    case DeclUsage::Normal:
    case DeclUsage::Tangent:
    case DeclUsage::Binormal:
        specConstantsMask |= SPEC_CONSTANT_R11G11B10_NORMAL;
        print("tfetchR11G11B10(");
        break;

    case DeclUsage::TexCoord:
        print("tfetchTexcoord(g_SwappedTexcoords, ");
        break;
    }
#endif

#ifdef RENUT_RECOMP
    // Recorded before the layout below reads it, so a full fetch describes
    // itself and every mini fetch that follows it.
    if (!instr.isMiniFetch)
    {
        lastFullFetch = instr;
        haveLastFullFetch = true;
    }


    // Export the layout while both halves of it are in hand: the element names
    // the semantic, the instruction carries the format, the offset, the stride
    // and the stream. A host that gets no vertex declaration from the game
    // rebuilds its input layout from this - see ShaderInputElement.
    //
    // First fetch of a semantic wins, matching the declaration loop's own
    // deduplication: several vfetches can read the same input, and the
    // signature declares it once.
    {
        const uint32_t usage = uint32_t(findResult->second.usage);
        const uint32_t usageIndex = uint32_t(findResult->second.usageIndex);

        auto existing = std::find_if(inputLayout.begin(), inputLayout.end(),
            [&](const ShaderInputElement& element)
            {
                return element.usage == usage && element.usageIndex == usageIndex;
            });

        // A semantic already described by a fetch that knew its stride is
        // settled. One described by a mini fetch that had no full fetch before
        // it - which happens when control flow emits the branch holding the
        // mini ahead of the one holding the full - is a placeholder, and is
        // replaced as soon as a fetch that does know arrives.
        const bool describesLayout = uint32_t(instr.stride) != 0 ||
            (instr.isMiniFetch && haveLastFullFetch && uint32_t(lastFullFetch.stride) != 0);

        if (existing == inputLayout.end() ||
            (existing->strideBytes == 0 && describesLayout))
        {
            // A mini fetch describes only where to put the result; everything
            // about *what* it reads comes from the full fetch it follows.
            const VertexFetchInstruction& source =
                (instr.isMiniFetch && haveLastFullFetch) ? lastFullFetch : instr;

            const uint32_t fetchConstant =
                uint32_t(source.constIndex) * 3 + uint32_t(source.constIndexSelect);

            ShaderInputElement element{};
            element.usage = uint8_t(usage);
            element.usageIndex = uint8_t(usageIndex);
            element.location = 0xFF;
            element.slot = fetchConstant <= 95 ? uint8_t(95 - fetchConstant) : 0xFF;

            // The offset is the instruction's own even for a mini fetch - that
            // is the whole point of one, reading a different field of the same
            // vertex.
            element.offsetBytes = uint16_t(uint32_t(instr.offset) * 4);
            element.fetchAddress = uint16_t(address);
            element.dstSwizzle = uint16_t(instr.dstSwizzle);
            element.strideBytes = uint16_t(uint32_t(source.stride) * 4);
            element.format = uint8_t(source.format);

            // numFormatAll clear means the components are fractions - the
            // normalised reading - and set means raw integers. Signedness is
            // formatCompAll; signedRfModeAll only picks how a signed fraction
            // maps to [-1, 1] (xenia's is_signed() / signed_rf_mode()).
            element.isNormalized = source.numFormatAll ? 0 : 1;
            element.isSigned = source.formatCompAll ? 1 : 0;

            for (auto& usageLocation : USAGE_LOCATIONS)
            {
                if (uint32_t(usageLocation.usage) == usage && usageLocation.usageIndex == usageIndex)
                {
                    element.location = uint8_t(usageLocation.location);
                    break;
                }
            }

            if (element.location == 0xFF || element.slot == 0xFF)
            {
                error("vertex input {}{} has no location or no stream to read it from",
                    USAGE_SEMANTICS[usage], usageIndex);
            }
            else if (existing != inputLayout.end())
            {
                *existing = element;
            }
            else
            {
                inputLayout.push_back(element);
            }
        }
    }
#endif

#ifdef RENUT_RECOMP
    // Formats with no host vertex format (2_10_10_10) arrive as raw bits;
    // unpackInput() decodes them for the locations the host flags.
    {
        uint32_t inputLocation = 0xFF;
        for (auto& usageLocation : USAGE_LOCATIONS)
        {
            if (usageLocation.usage == findResult->second.usage &&
                usageLocation.usageIndex == uint32_t(findResult->second.usageIndex))
            {
                inputLocation = usageLocation.location;
                break;
            }
        }
        print("unpackInput(i{}{}, {})", USAGE_VARIABLES[uint32_t(findResult->second.usage)],
              uint32_t(findResult->second.usageIndex), inputLocation);
    }
#else
    print("i{}{}", USAGE_VARIABLES[uint32_t(findResult->second.usage)], uint32_t(findResult->second.usageIndex));
#endif

#ifdef RENUT_RECOMP
    switch (findResult->second.usage)
    {
    case DeclUsage::Normal:
    case DeclUsage::Tangent:
    case DeclUsage::Binormal:
        print(", {}))", uint32_t(findResult->second.usageIndex));
        break;

    case DeclUsage::TexCoord:
        print(", {}), {})", uint32_t(findResult->second.usageIndex),
              uint32_t(findResult->second.usageIndex));
        break;

    case DeclUsage::BlendWeight:
    case DeclUsage::BlendIndices:
    case DeclUsage::Position:
        print(", {})", uint32_t(findResult->second.usageIndex));
        break;
    }
#else
    switch (findResult->second.usage)
    {
    case DeclUsage::Normal:
    case DeclUsage::Tangent:
    case DeclUsage::Binormal:
        out += ')';
        break;

    case DeclUsage::TexCoord:
        print(", {})", uint32_t(findResult->second.usageIndex));
        break;
    }
#endif

    out += '.';
    printDstSwizzle(instr.dstSwizzle, true);

    out += ";\n";

    printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);

    if (instr.isPredicated)
    {
        --indentation;
        indent();
        out += "}\n";
    }
}


void ShaderRecompiler::recompile(const TextureFetchInstruction& instr, bool /*bicubic*/)
{
    switch (instr.opcode)
    {
    case FetchOpcode::TextureFetch:
    case FetchOpcode::GetTextureWeights:
    case FetchOpcode::GetTextureComputedLod:
        break;

    // These configure state for the fetches that follow rather than producing
    // a value. N&B uses SetTextureLod in four vertex shaders; the emitted
    // fetches always sample the base level, so honouring it would need an
    // explicit-LOD variant of every tfetch. Note it and move on.
    case FetchOpcode::SetTextureLod:
    case FetchOpcode::SetTextureGradientsHorz:
    case FetchOpcode::SetTextureGradientsVert:
    case FetchOpcode::GetTextureBorderColorFrac:
    case FetchOpcode::GetTextureGradients:
        indent();
        println("// unimplemented fetch opcode {} (slot {})", uint32_t(instr.opcode), uint32_t(instr.constIndex));
        return;

    default:
        error("unknown fetch opcode {}", uint32_t(instr.opcode));
        return;
    }

    if (instr.isPredicated)
    {
        indent();
        println("if ({}p0)", instr.predCondition ? "" : "!");

        indent();
        out += "{\n";
        ++indentation;
    }

    auto printSrcRegister = [&](size_t componentCount)
        {
            print("r{}.", instr.srcRegister);

            for (size_t i = 0; i < componentCount; i++)
                out += SWIZZLES[((instr.srcSwizzle >> (i * 2))) & 0x3];
        };

    // Nothing names samplers here: almost every N&B shader arrives without a
    // constant table, so the fetch slot index is the only handle there is and
    // the descriptor lookup is a shared accessor rather than a per-shader
    // "<name>_Texture2DDescriptorIndex" define. Where a table does name the
    // slot, the comment block at the top of the shader records it.
    const uint32_t slot = instr.constIndex;
    if (slot >= RENUT_TEXTURE_SLOT_COUNT)
    {
        error("texture fetch slot {} is past the {} the shared constants describe",
            slot, uint32_t(RENUT_TEXTURE_SLOT_COUNT));
        return;
    }

    indent();
#ifdef RENUT_RECOMP
    markWritten(uint32_t(instr.dstRegister));
#endif
    print("r{}.", instr.dstRegister);
    printDstSwizzle(instr.dstSwizzle, false);

    out += " = ";

    std::string_view dimension;
    uint32_t componentCount = 0;

    switch (instr.dimension)
    {
    case TextureDimension::Texture1D:
        dimension = "1D";
        componentCount = 1;
        break;
    case TextureDimension::Texture2D:
        dimension = "2D";
        componentCount = 2;
        break;
    case TextureDimension::Texture3D:
        dimension = "3D";
        componentCount = 3;
        break;
    case TextureDimension::TextureCube:
        dimension = "Cube";
        componentCount = 3;
        break;
    }

    // A Xenos 1D texture is a one-row 2D surface and the host binds it in the
    // Texture2D heap, so the 1D helpers take the 2D descriptor.
    const std::string_view heapDimension = (instr.dimension == TextureDimension::Texture1D) ? "2D" : dimension;

    if (instr.opcode == FetchOpcode::GetTextureComputedLod)
    {
        // Only ever meaningful against a 2D surface, which is all N&B asks
        // for; the result is a scalar broadcast so the destination swizzle
        // below works the same way it does for a sample.
        // Parenthesised so the destination swizzle appended below lands on the
        // float4, not on the scalar the function returns - a fetch writing more
        // than one component would otherwise swizzle a float.
        print("((float4)computeMipLevel(getPixelCoord(texture2DDescriptorIndex({}), ", slot);
        printSrcRegister(2);
        out += ")))";
    }
    else
    {
        out += (instr.opcode == FetchOpcode::GetTextureWeights) ? "getWeights" : "tfetch";
        out += dimension;

        print("(texture{}DescriptorIndex({}), samplerDescriptorIndex({}), ", heapDimension, slot, slot);
        // tx_coord_denorm: the coordinates are in texels. N&B's soft particles
        // read the resolved scene depth at the pixel position this way; taken
        // as fractions they all read the clamped edge and faded to nothing.
        const bool denorm = instr.texCoordDenorm &&
            (instr.dimension == TextureDimension::Texture1D || instr.dimension == TextureDimension::Texture2D);
        if (denorm)
            out += "(";
        printSrcRegister(componentCount);
        if (denorm)
        {
            print(") / float{}(getTexture2DDimensions(g_Texture2DDescriptorHeap[texture2DDescriptorIndex({})]){})",
                componentCount == 1 ? "" : "2", slot, componentCount == 1 ? ".x" : "");
        }

        switch (instr.dimension)
        {
        case TextureDimension::Texture1D:
        case TextureDimension::Texture2D:
            print(", float2({}, {})", instr.offsetX * 0.5f, instr.offsetY * 0.5f);
            break;
        case TextureDimension::TextureCube:
            out += ", cubeMapData";
            break;
        }

        out += ")";
    }

    out += ".";

    printDstSwizzle(instr.dstSwizzle, true);

    out += ";\n";

    printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);

    if (instr.isPredicated)
    {
        --indentation;
        indent();
        out += "}\n";
    }
}

void ShaderRecompiler::recompile(const AluInstruction& instr)
{
    if (instr.isPredicated)
    {
        indent();
        println("if ({}p0)", instr.predicateCondition ? "" : "!");

        indent();
        out += "{\n";
        ++indentation;
    }

    enum
    {
        VECTOR_0,
        VECTOR_1,
        VECTOR_2,
        SCALAR_0,
        SCALAR_1,
        SCALAR_CONSTANT_0,
        SCALAR_CONSTANT_1
    };

    // On an export the scalar half of the instruction claims its lanes from
    // the vector half, and the operand swizzle has to be built from what is
    // left rather than from the raw mask - otherwise a destination like
    // "oC0.xzw" would be fed the source's first three lanes (x, y, z).
    const uint32_t exportedVectorWriteMask = instr.exportData
        ? (instr.vectorWriteMask & ~instr.scalarWriteMask)
        : uint32_t(instr.vectorWriteMask);

    auto op = [&](size_t operand, bool fullVectorSwizzle = false)
        {
            size_t reg = 0;
            size_t swizzle = 0;
            bool select = true;
            bool negate = false;
            bool abs = false;

            switch (operand)
            {
            case SCALAR_CONSTANT_0:
                reg = instr.src3Register;
                swizzle = instr.src3Swizzle;
                select = false;
                negate = instr.src3Negate;
                abs = instr.absConstants;
                break;

            case SCALAR_CONSTANT_1:
                reg = (uint32_t(instr.scalarOpcode) & 1) | (instr.src3Select << 1) | (instr.src3Swizzle & 0x3C);
                swizzle = instr.src3Swizzle;
                select = true;
                negate = instr.src3Negate;
                abs = instr.absConstants;
                break;

            default:
                switch (operand)
                {
                case VECTOR_0:
                    reg = instr.src1Register;
                    swizzle = instr.src1Swizzle;
                    select = instr.src1Select;
                    negate = instr.src1Negate;
                    break;
                case VECTOR_1:
                    reg = instr.src2Register;
                    swizzle = instr.src2Swizzle;
                    select = instr.src2Select;
                    negate = instr.src2Negate;
                    break;
                case VECTOR_2:
                case SCALAR_0:
                case SCALAR_1:
                    reg = instr.src3Register;
                    swizzle = instr.src3Swizzle;
                    select = instr.src3Select;
                    negate = instr.src3Negate;
                    break;
                }

                if (select)
                {
                    abs = (reg & 0x80) != 0;
                    reg &= 0x3F;
                }
                else
                {
                    abs = instr.absConstants;
                }

                break;
            }

            std::string regFormatted;

            if (select)
            {
                regFormatted = fmt::format("r{}", reg);
            }
#ifdef RENUT_RECOMP
            else
            {
                // Xenos gives an ALU op two constant slots, not two constant
                // operands: source 1 always reads slot 0, and sources 2 and 3
                // read slot 0 only while every earlier source is a temp. The
                // relative-addressing bit that applies is the one for that
                // slot, which the old "always const0Relative" reading got
                // backwards on N&B's mad/mul-with-two-constants forms.
                uint32_t constSlot = 0;
                switch (operand)
                {
                case VECTOR_1:
                    constSlot = instr.src1Select ? 0 : 1;
                    break;
                case VECTOR_2:
                    constSlot = (instr.src1Select && instr.src2Select) ? 0 : 1;
                    break;
                default:
                    constSlot = 0;
                    break;
                }

                const bool relative = (constSlot == 0) ? bool(instr.const0Relative) : bool(instr.const1Relative);

                if (relative)
                {
                    // A vertex shader's relative reads go through cRegRel, which
                    // the host can serve from one contiguous block (see
                    // g_VertexShaderConstantRel); the flag tells it to place one.
                    regFormatted = fmt::format("{}({} + {})", isPixelShader ? "cReg" : "cRegRel", reg,
                        instr.constAddressRegisterRelative ? "a0" : "aL");
                    if (!isPixelShader)
                        specConstantsMask |= RENUT_SHADER_FLAG_REL_CONSTANTS;
                }
                else if (literalConstants.count(reg) != 0)
                {
                    // Initialised by the shader's own float4 definition table,
                    // which the runtime writes into the register file when the
                    // shader is set, so the literal is what the body sees.
                    regFormatted = fmt::format("c{}", reg);
                }
                else
                {
                    regFormatted = fmt::format("cReg({})", reg);
                }
            }
#else
            else
            {
                auto findResult = float4Constants.find(reg);
                if (findResult != float4Constants.end())
                {
                    const char* constantName = reinterpret_cast<const char*>(constantTableData + findResult->second->name);
                    if (findResult->second->registerCount > 1)
                    {
                    #ifdef UNLEASHED_RECOMP
                        if (hasMtxProjection && strcmp(constantName, "g_MtxProjection") == 0)
                        {
                            regFormatted = fmt::format("(iterationIndex == 0 ? mtxProjectionReverseZ[{0}] : mtxProjection[{0}])",
                                reg - findResult->second->registerIndex);
                        }
                        else
                    #endif
                        {
                            regFormatted = fmt::format("{}({}{})", constantName,
                                reg - findResult->second->registerIndex, instr.const0Relative ? (instr.constAddressRegisterRelative ? " + a0" : " + aL") : "");
                        }
                    }
                    else
                    {
                        assert(!instr.const0Relative && !instr.const1Relative);
                        regFormatted = constantName;
                    }
                }
                else
                {
                    assert(!instr.const0Relative && !instr.const1Relative);
                    regFormatted = fmt::format("c{}", reg);
                }
            }
#endif

            std::string result;

            if (negate)
                result += '-';

            if (abs)
                result += "abs(";

            result += regFormatted;
            result += '.';

            switch (operand)
            {
            case VECTOR_0:
            case VECTOR_1:
            case VECTOR_2:
            {
                uint32_t mask;

                if (fullVectorSwizzle)
                {
                    mask = 0b1111;
                }
                else switch (instr.vectorOpcode)
                {
                case AluVectorOpcode::Dp2Add:
                    mask = (operand == VECTOR_2) ? 0b1 : 0b11;
                    break;

                case AluVectorOpcode::Dp3:
                    mask = 0b111;
                    break;

                case AluVectorOpcode::Dp4:
                case AluVectorOpcode::Max4:
                    mask = 0b1111;
                    break;

                default:
                    mask = exportedVectorWriteMask != 0 ? exportedVectorWriteMask : 0b1;
                    break;
                }

                for (size_t i = 0; i < 4; i++)
                {
                    if ((mask >> i) & 0x1)
                        result += SWIZZLES[((swizzle >> (i * 2)) + i) & 0x3];
                }

                break;
            }

            case SCALAR_0:
            case SCALAR_CONSTANT_0:
                result += SWIZZLES[((swizzle >> 6) + 3) & 0x3];
                break;

            case SCALAR_1:
            case SCALAR_CONSTANT_1:
                result += SWIZZLES[swizzle & 0x3];
                break;
            }

            if (abs)
                result += ")";

            return result;
        };

    switch (instr.vectorOpcode)
    {
    case AluVectorOpcode::KillEq:
        indent();
        println("clip(any({} == {}) ? -1 : 1);", op(VECTOR_0, true), op(VECTOR_1, true));
        break;

    case AluVectorOpcode::KillGt:
        indent();
        println("clip(any({} > {}) ? -1 : 1);", op(VECTOR_0, true), op(VECTOR_1, true));
        break;

    case AluVectorOpcode::KillGe:
        indent();
        println("clip(any({} >= {}) ? -1 : 1);", op(VECTOR_0, true), op(VECTOR_1, true));
        break;

    case AluVectorOpcode::KillNe:
        indent();
        println("clip(any({} != {}) ? -1 : 1);", op(VECTOR_0, true), op(VECTOR_1, true));
        break;
    }

    bool closeIfBracket = false;

    std::string_view exportRegister;
    if (instr.exportData)
    {
        if (isPixelShader)
        {
            uint32_t requiredOutput = 0;

            switch (ExportRegister(instr.vectorDest))
            {
            case ExportRegister::PSColor0:
                exportRegister = "oC0";
                requiredOutput = PIXEL_SHADER_OUTPUT_COLOR0;
                break;
            case ExportRegister::PSColor1:
                exportRegister = "oC1";
                requiredOutput = PIXEL_SHADER_OUTPUT_COLOR1;
                break;
            case ExportRegister::PSColor2:
                exportRegister = "oC2";
                requiredOutput = PIXEL_SHADER_OUTPUT_COLOR2;
                break;
            case ExportRegister::PSColor3:
                exportRegister = "oC3";
                requiredOutput = PIXEL_SHADER_OUTPUT_COLOR3;
                break;
            case ExportRegister::PSDepth:
                exportRegister = "oDepth";
                requiredOutput = PIXEL_SHADER_OUTPUT_DEPTH;
                break;
            }

#ifdef RENUT_RECOMP
            // main() only declares the targets the shader header advertises,
            // so an export to any other one would name a missing parameter.
            if (requiredOutput != 0 && (pixelShaderOutputs & requiredOutput) == 0)
            {
                error("pixel shader exports to register {}, which its header does not declare",
                    uint32_t(instr.vectorDest));
                return;
            }
#endif
        }
        else
        {
            switch (ExportRegister(instr.vectorDest))
            {
            case ExportRegister::VSPosition:
                exportRegister = "oPos";

            #ifdef UNLEASHED_RECOMP
                if (hasMtxProjection)
                {
                    indent();
                    out += "if ((g_SpecConstants() & SPEC_CONSTANT_REVERSE_Z) == 0 || iterationIndex == 0)\n";
                    indent();
                    out += "{\n";
                    ++indentation;

                    closeIfBracket = true;
                }
            #endif

                break;

            default:
            {
#ifdef RENUT_RECOMP
                switch (ExportRegister(instr.vectorDest))
                {
                // Point size (.x), edge flag (.y) and kill vertex (.z). N&B
                // writes .x in 14 shaders and .z in 14 more, so it cannot just
                // be dropped: the kill is applied at the end of the shader and
                // the point size is kept for a host that can consume it.
                case ExportRegister::VSPointSizeEdgeFlagKillVertex:
                    exportRegister = "oPointSizeEdgeKill";
                    break;

                // Memory export. The data registers collect the elements, and
                // they are stored when the address is written again or the
                // shader ends - the address in effect is the one they were
                // written under (renutMemExport in shader_common.h).
                case ExportRegister::ExportAddress:
                    indent();
                    out += "renutMemExport(eA, eM0, eM1, eM2, eM3, eM4, eMWritten);\n";
                    indent();
                    out += "eMWritten = 0;\n";
                    exportRegister = "eA";
                    specConstantsMask |= RENUT_SHADER_FLAG_MEMEXPORT;
                    break;
                case ExportRegister::ExportData0:
                case ExportRegister::ExportData1:
                case ExportRegister::ExportData2:
                case ExportRegister::ExportData3:
                case ExportRegister::ExportData4:
                {
                    static constexpr std::string_view kDataRegisters[] = {"eM0", "eM1", "eM2", "eM3", "eM4"};
                    const uint32_t element = uint32_t(instr.vectorDest) - uint32_t(ExportRegister::ExportData0);
                    indent();
                    println("eMWritten |= {}u;", 1u << element);
                    exportRegister = kDataRegisters[element];
                    specConstantsMask |= RENUT_SHADER_FLAG_MEMEXPORT;
                    break;
                }

                default:
                {
                    auto findResult = interpolators.find(instr.vectorDest);
                    if (findResult == interpolators.end())
                    {
                        error("vertex shader exports to unmapped register {}", uint32_t(instr.vectorDest));
                        return;
                    }
                    exportRegister = findResult->second;
                    break;
                }
                }
#else
                auto findResult = interpolators.find(instr.vectorDest);
                assert(findResult != interpolators.end());
                exportRegister = findResult->second;
#endif
                break;
            }
            }
        }
    }

    if (instr.vectorOpcode >= AluVectorOpcode::SetpEqPush && instr.vectorOpcode <= AluVectorOpcode::SetpGePush)
    {
        indent();
#ifdef RENUT_RECOMP
        // The predicate tests the sources' w, whatever the destination mask
        // (see the result below for x). Taking the component the mask selects
        // tested the wrong lanes for any destination but .w.
        print("p0 = ({}).w == 0.0 && ({}).w ", op(VECTOR_0, true), op(VECTOR_1, true));
#else
        print("p0 = {} == 0.0 && {} ", op(VECTOR_0), op(VECTOR_1));
#endif

        switch (instr.vectorOpcode)
        {
        case AluVectorOpcode::SetpEqPush:
            out += "==";
            break;
        case AluVectorOpcode::SetpNePush:
            out += "!=";
            break;
        case AluVectorOpcode::SetpGtPush:
            out += ">";
            break;
        case AluVectorOpcode::SetpGePush:
            out += ">=";
            break;
        }

        out += " 0.0;\n";
    }
    else if (instr.vectorOpcode >= AluVectorOpcode::MaxA)
    {
        indent();
        println("a0 = (int)clamp(floor(({}).w + 0.5), -256.0, 255.0);", op(VECTOR_0, true));
    }

    uint32_t vectorWriteMask = exportedVectorWriteMask;

#ifdef RENUT_RECOMP
    // oDepth is a single float, so a write mask covering the whole register
    // would swizzle past it. The hardware takes depth from lane x, and the
    // shaders that write all four lanes broadcast the same value into them.
    const bool isDepthExport = (exportRegister == "oDepth");
    if (isDepthExport)
        vectorWriteMask &= 0b1;
#endif

    // The vector and scalar halves of an ALU instruction run in parallel: both
    // read their sources before either writes. Written straight away, a vector
    // result would reach a scalar operation that reads the same register (N&B's
    // particle shaders take a lifetime as end - birth in the scalar half while
    // the vector half overwrites end with the age), so it waits in a temporary.
    const bool deferVector = vectorWriteMask != 0 && exportRegister.empty() &&
        instr.scalarOpcode != AluScalarOpcode::RetainPrev;
    if (vectorWriteMask != 0)
    {
        indent();
        if (deferVector)
        {
            out += "{\n";
            ++indentation;
            indent();
            const uint32_t lanes = uint32_t(__builtin_popcount(vectorWriteMask));
            if (lanes == 1)
                out += "float vectorResult";
            else
                print("float{} vectorResult", lanes);
        }
        else if (!exportRegister.empty())
        {
            out += exportRegister;
            out += '.';
        }
        else
        {
#ifdef RENUT_RECOMP
            markWritten(uint32_t(instr.vectorDest));
#endif
            print("r{}.", instr.vectorDest);
        }

        for (size_t i = 0; i < 4 && !deferVector; i++)
        {
            if ((vectorWriteMask >> i) & 0x1)
                out += SWIZZLES[i];
        }

        out += " = ";

        if (instr.vectorSaturate)
            out += "saturate(";

        switch (instr.vectorOpcode)
        {
        case AluVectorOpcode::Add:
            print("{} + {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Mul:
            print("{} * {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Max:
        case AluVectorOpcode::MaxA:
            print("max({}, {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Min:
            print("min({}, {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Seq:
            print("{} == {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Sgt:
            print("{} > {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Sge:
            print("{} >= {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Sne:
            print("{} != {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Frc:
            print("frac({})", op(VECTOR_0));
            break;

        case AluVectorOpcode::Trunc:
            print("trunc({})", op(VECTOR_0));
            break;

        case AluVectorOpcode::Floor:
            print("floor({})", op(VECTOR_0));
            break;

        case AluVectorOpcode::Mad:
            print("{} * {} + {}", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::CndEq:
            print("select({} == 0.0, {}, {})", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::CndGe:
            print("select({} >= 0.0, {}, {})", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::CndGt:
            print("select({} > 0.0, {}, {})", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::Dp4:
        case AluVectorOpcode::Dp3:
            print("dot({}, {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Dp2Add:
            print("dot({}, {}) + {}", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::Cube:
        {
            // Xenos cube takes the direction from src1's swizzled lanes (z, w, x);
            // the canonical emit is src.zzxy which makes this src.xyz, but other
            // compilers emit e.g. src.yyzx (Blue Dragon), so honor the swizzle.
            const uint32_t reg = instr.src1Register & 0x3F;
            const bool srcAbs = (instr.src1Register & 0x80) != 0;
            auto lane = [&](uint32_t i) { return SWIZZLES[((instr.src1Swizzle >> (i * 2)) + i) & 0x3]; };
            print("cube({}{}r{}.{}{}{}{}{}, cubeMapData)",
                instr.src1Negate ? "-" : "", srcAbs ? "abs(" : "", reg,
                lane(2), lane(3), lane(0), lane(0), srcAbs ? ")" : "");
            break;
        }

        case AluVectorOpcode::Max4:
            print("max4({})", op(VECTOR_0));
            break;

        case AluVectorOpcode::SetpEqPush:
        case AluVectorOpcode::SetpNePush:
        case AluVectorOpcode::SetpGtPush:
        case AluVectorOpcode::SetpGePush:
#ifdef RENUT_RECOMP
        {
            // Xenos: the result has its own test, on x - dest = (src0.x == 0 &&
            // src1.x <cmp> 0) ? 0 : src0.x + 1 - while p0 tests w. Reusing p0
            // and adding 1 to the masked lane broke the counters N&B's lit
            // shaders pick their shadow cascade with: building shadows cut
            // off just past the player at night.
            const char* compare = instr.vectorOpcode == AluVectorOpcode::SetpEqPush ? "==" :
                                  instr.vectorOpcode == AluVectorOpcode::SetpNePush ? "!=" :
                                  instr.vectorOpcode == AluVectorOpcode::SetpGtPush ? ">" : ">=";
            print("(({0}).x == 0.0 && ({1}).x {2} 0.0) ? 0.0 : ({0}).x + 1.0", op(VECTOR_0, true), op(VECTOR_1, true), compare);
            break;
        }
#else
            print("p0 ? 0.0 : {} + 1.0", op(VECTOR_0));
            break;
#endif

        case AluVectorOpcode::KillEq:
            print("any({} == {})", op(VECTOR_0, true), op(VECTOR_1, true));
            break;

        case AluVectorOpcode::KillGt:
            print("any({} > {})", op(VECTOR_0, true), op(VECTOR_1, true));
            break;

        case AluVectorOpcode::KillGe:
            print("any({} >= {})", op(VECTOR_0, true), op(VECTOR_1, true));
            break;

        case AluVectorOpcode::KillNe:
            print("any({} != {})", op(VECTOR_0, true), op(VECTOR_1, true));
            break;

        case AluVectorOpcode::Dst:
            print("dst({}, {})", op(VECTOR_0, true), op(VECTOR_1, true));
            break;
        }

        if (instr.vectorSaturate)
            out += ')';

        out += ";\n";
    }

    if (instr.scalarOpcode != AluScalarOpcode::RetainPrev)
    {
        if (instr.scalarOpcode >= AluScalarOpcode::SetpEq && instr.scalarOpcode <= AluScalarOpcode::SetpRstr)
        {
            indent();
            out += "p0 = ";

            switch (instr.scalarOpcode)
            {
            case AluScalarOpcode::SetpEq:
                print("{} == 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpNe:
                print("{} != 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpGt:
                print("{} > 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpGe:
                print("{} >= 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpInv:
                print("{} == 1.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpPop:
                print("{} - 1.0 <= 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpClr:
                out += "false";
                break;

            case AluScalarOpcode::SetpRstr:
                print("{} == 0.0", op(SCALAR_0));
                break;
            }

            out += ";\n";
        }

        indent();
        out += "ps = ";
        if (instr.scalarSaturate)
            out += "saturate(";

        switch (instr.scalarOpcode)
        {
        case AluScalarOpcode::Adds:
            print("{} + {}", op(SCALAR_0), op(SCALAR_1));
            break;

        case AluScalarOpcode::AddsPrev:
            print("{} + ps", op(SCALAR_0));
            break;

        case AluScalarOpcode::Muls:
            print("{} * {}", op(SCALAR_0), op(SCALAR_1));
            break;

        case AluScalarOpcode::MulsPrev:
        case AluScalarOpcode::MulsPrev2:
            print("{} * ps", op(SCALAR_0));
            break;

        case AluScalarOpcode::Maxs:
        case AluScalarOpcode::MaxAs:
        case AluScalarOpcode::MaxAsf:
            print("max({}, {})", op(SCALAR_0), op(SCALAR_1));
            break;

        case AluScalarOpcode::Mins:
            print("min({}, {})", op(SCALAR_0), op(SCALAR_1));
            break;

        case AluScalarOpcode::Seqs:
            print("{} == 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Sgts:
            print("{} > 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Sges:
            print("{} >= 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Snes:
            print("{} != 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Frcs:
            print("frac({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Truncs:
            print("trunc({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Floors:
            print("floor({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Exp:
            print("exp2({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Logc:
        case AluScalarOpcode::Log:
            print("clamp(log2({}), FLT_MIN, FLT_MAX)", op(SCALAR_0));
            break;

        case AluScalarOpcode::Rcpc:
        case AluScalarOpcode::Rcpf:
        case AluScalarOpcode::Rcp:
            print("clamp(rcp({}), FLT_MIN, FLT_MAX)", op(SCALAR_0));
            break;

        case AluScalarOpcode::Rsqc:
        case AluScalarOpcode::Rsqf:
        case AluScalarOpcode::Rsq:
            print("clamp(rsqrt({}), FLT_MIN, FLT_MAX)", op(SCALAR_0));
            break;

        case AluScalarOpcode::Subs:
            print("{} - {}", op(SCALAR_0), op(SCALAR_1));
            break;

        case AluScalarOpcode::SubsPrev:
            print("{} - ps", op(SCALAR_0));
            break;

        case AluScalarOpcode::SetpEq:
        case AluScalarOpcode::SetpNe:
        case AluScalarOpcode::SetpGt:
        case AluScalarOpcode::SetpGe:
            out += "p0 ? 0.0 : 1.0";
            break;

        case AluScalarOpcode::SetpInv:
#ifdef RENUT_RECOMP
            // PRED_SETINV: src==1 -> 0, src==0 -> 1, else passthrough.
            print("{0} == 1.0 ? 0.0 : ({0} == 0.0 ? 1.0 : {0})", op(SCALAR_0));
#else
            print("{0} == 0.0 ? 1.0 : {0}", op(SCALAR_0));
#endif
            break;

        case AluScalarOpcode::SetpPop:
            print("p0 ? 0.0 : ({} - 1.0)", op(SCALAR_0));
            break;

        case AluScalarOpcode::SetpClr:
            out += "FLT_MAX";
            break;

        case AluScalarOpcode::SetpRstr:
            print("p0 ? 0.0 : {}", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsEq:
            print("{} == 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsGt:
            print("{} > 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsGe:
            print("{} >= 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsNe:
            print("{} != 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsOne:
            print("{} == 1.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Sqrt:
            print("sqrt({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Mulsc0:
        case AluScalarOpcode::Mulsc1:
            print("{} * {}", op(SCALAR_CONSTANT_0), op(SCALAR_CONSTANT_1));
            break;

        case AluScalarOpcode::Addsc0:
        case AluScalarOpcode::Addsc1:
            print("{} + {}", op(SCALAR_CONSTANT_0), op(SCALAR_CONSTANT_1));
            break;

        case AluScalarOpcode::Subsc0:
        case AluScalarOpcode::Subsc1:
            print("{} - {}", op(SCALAR_CONSTANT_0), op(SCALAR_CONSTANT_1));
            break;

        case AluScalarOpcode::Sin:
            print("sin({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Cos:
            print("cos({})", op(SCALAR_0));
            break;
        }

        if (instr.scalarSaturate)
            out += ')';

        out += ";\n";

        switch (instr.scalarOpcode)
        {
        case AluScalarOpcode::MaxAs:
            indent();
            println("a0 = (int)clamp(floor({} + 0.5), -256.0, 255.0);", op(SCALAR_0));
            break;
        case AluScalarOpcode::MaxAsf:
            indent();
            println("a0 = (int)clamp(floor({}), -256.0, 255.0);", op(SCALAR_0));
            break;
        }
    }

    if (deferVector)
    {
        indent();
#ifdef RENUT_RECOMP
        markWritten(uint32_t(instr.vectorDest));
#endif
        print("r{}.", instr.vectorDest);
        for (size_t i = 0; i < 4; i++)
        {
            if ((vectorWriteMask >> i) & 0x1)
                out += SWIZZLES[i];
        }
        out += " = vectorResult;\n";
        --indentation;
        indent();
        out += "}\n";
    }

    uint32_t scalarWriteMask = instr.scalarWriteMask;
    if (instr.exportData)
        scalarWriteMask &= ~instr.vectorWriteMask;

#ifdef RENUT_RECOMP
    if (isDepthExport)
        scalarWriteMask &= 0b1;
#endif

    if (scalarWriteMask != 0)
    {
        indent();
        if (!exportRegister.empty())
        {
            out += exportRegister;
            out += '.';
        }
        else
        {
#ifdef RENUT_RECOMP
            markWritten(uint32_t(instr.scalarDest));
#endif
            print("r{}.", instr.scalarDest);
        }

        for (size_t i = 0; i < 4; i++)
        {
            if ((scalarWriteMask >> i) & 0x1)
                out += SWIZZLES[i];
        }

        out += " = ps;\n";
    }

    if (instr.exportData)
    {
        uint32_t zeroMask = instr.scalarDestRelative ? (0b1111 & ~(instr.vectorWriteMask | instr.scalarWriteMask)) : 0;
        uint32_t oneMask = instr.vectorWriteMask & instr.scalarWriteMask;

#ifdef RENUT_RECOMP
        if (isDepthExport)
        {
            zeroMask &= 0b1;
            oneMask &= 0b1;
        }
#endif

        for (size_t i = 0; i < 4; i++)
        {
            uint32_t mask = 1 << i;
            if (zeroMask & mask)
            {
                indent();
                println("{}.{} = 0.0;", exportRegister, SWIZZLES[i]);
            }
            else if (oneMask & mask)
            {
                indent();
                println("{}.{} = 1.0;", exportRegister, SWIZZLES[i]);
            }
        }
    }

    if (instr.scalarOpcode >= AluScalarOpcode::KillsEq && instr.scalarOpcode <= AluScalarOpcode::KillsOne)
    {
        indent();
        out += "clip(ps != 0.0 ? -1 : 1);\n";
    }

    if (closeIfBracket)
    {
        --indentation;
        indent();
        out += "}\n";
    }

    if (instr.isPredicated)
    {
        --indentation;
        indent();
        out += "}\n";
    }
}

void ShaderRecompiler::recompile(const uint8_t* shaderData, const std::string_view& include)
{
    const auto shaderContainer = reinterpret_cast<const ShaderContainer*>(shaderData);

    assert((shaderContainer->flags & 0xFFFFFF00) == 0x102A1100);

    isPixelShader = (shaderContainer->flags & 0x1) == 0;

#ifdef RENUT_RECOMP
    if (isPixelShader)
        isDomainShader = false;
    // The tessellator hands the domain stage its barycentric coordinates in r0
    // and the patch index in r1, so a vfetch indexed by either is not indexed
    // by a vertex id and cannot be an input assembler attribute. Marking them
    // written up front sends every one of them down the vfetchLive path, which
    // reads the stream directly.
    if (isDomainShader)
    {
        markWritten(0);
        markWritten(1);
    }
    // shader_common.h picks derivative-based or explicit-level sampling from
    // this, so it has to land ahead of the include rather than after it. A
    // domain shader has no derivatives either, so it reads as a vertex stage.
    println("#define RENUT_STAGE_{}", isPixelShader ? "PIXEL" : "VERTEX");
#endif

    out += include;
    out += '\n';

    // Nuts & Bolts hands the driver bare microcode: 239 of the 256 shaders in
    // the dump have constantTableOffset == 0, so there is nothing naming their
    // constants, samplers or booleans. Everything below therefore addresses
    // those files positionally the way the hardware does, and a constant table,
    // when one is present, only contributes the comment block.
    const ConstantTableContainer* constantTableContainer = nullptr;
    uint32_t constantCount = 0;

    if (shaderContainer->constantTableOffset != NULL)
    {
        constantTableContainer = reinterpret_cast<const ConstantTableContainer*>(shaderData + shaderContainer->constantTableOffset);
        constantTableData = reinterpret_cast<const uint8_t*>(&constantTableContainer->constantTable);
        constantCount = constantTableContainer->constantTable.constants;
    }

    const char* stageName = isPixelShader ? "Pixel" : "Vertex";

    if (constantCount != 0)
    {
        out += "// constant table:\n";

        for (uint32_t i = 0; i < constantCount; i++)
        {
            const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
                constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

            const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);

            switch (constantInfo->registerSet)
            {
            case RegisterSet::Float4:
                println("//   c{}..c{}\t{}", constantInfo->registerIndex.get(),
                    constantInfo->registerIndex + constantInfo->registerCount - 1, constantName);

                for (uint16_t j = 0; j < constantInfo->registerCount; j++)
                    float4Constants.emplace(constantInfo->registerIndex + j, constantInfo);
                break;

            case RegisterSet::Sampler:
                println("//   s{}\t{}", constantInfo->registerIndex.get(), constantName);
                samplers.emplace(constantInfo->registerIndex, constantName);
                break;

            case RegisterSet::Bool:
                // The unified VS(0..127)/PS(128..255) bit, which is what
                // control flow tests against.
                println("//   b{}\t{}", constantInfo->registerIndex + (isPixelShader ? 128u : 0u), constantName);
                break;

            case RegisterSet::Int4:
                println("//   i{}\t{}", constantInfo->registerIndex.get(), constantName);
                break;
            }
        }

        out += '\n';
    }

    // The stage's whole float4 constant file, reached through cReg() from
    // shader_common.h. Aliasing the names would only help the handful of
    // shaders that carry a table, so the body always uses raw indices.
    out += "#ifdef __spirv__\n\n";
    println("#define RENUT_STAGE_CONSTANTS g_{}ShaderConstant", stageName);
    println("#define RENUT_STAGE_REL_CONSTANTS g_{}ShaderConstantRel", stageName);
    out += "\n#else\n\n";

    println("cbuffer {}ShaderConstants : register(b{}, space4)", stageName, isPixelShader ? 1 : 0);
    out += "{\n";
    out += "\tfloat4 g_ShaderConstants[RENUT_SHADER_CONSTANT_COUNT] : packoffset(c0);\n";
    out += "};\n\n";

    out += "cbuffer SharedConstants : register(b2, space4)\n";
    out += "{\n";
    out += "\tDEFINE_SHARED_CONSTANTS();\n";
    out += "};\n\n";

    out += "#endif\n";

    const auto shader = reinterpret_cast<const Shader*>(shaderData + shaderContainer->shaderOffset);

    out += "#ifndef __spirv__\n";

    if (isPixelShader)
        out += "[shader(\"pixel\")]\n";
    else
        out += "[shader(\"vertex\")]\n";

    out += "#endif\n";

    out += "void main(\n";

    if (isPixelShader)
    {
        out += "\tin float4 iPos : SV_Position,\n";

        for (auto& [usage, usageIndex] : INTERPOLATORS)
        {
#ifdef RENUT_RECOMP
            const char* interpolation = (usage == DeclUsage::Color) ? "centroid " : "";
#else
            const char* interpolation = "";
#endif
            println("\tin {3}float4 i{0}{1} : {2}{1},", USAGE_VARIABLES[uint32_t(usage)], usageIndex, USAGE_SEMANTICS[uint32_t(usage)], interpolation);
        }
#ifdef RENUT_RECOMP
        // Declared last so it takes RENUT_POINT_SPRITE_LOCATION.
        out += "\tin float4 iPointSprite : POINTSPRITE,\n";
#endif

        out += "#ifdef __spirv__\n";
        out += "\tin bool iFace : SV_IsFrontFace\n";
        out += "#else\n";
        out += "\tin uint iFace : SV_IsFrontFace\n";
        out += "#endif\n";

        auto pixelShader = reinterpret_cast<const PixelShader*>(shader);
#ifdef RENUT_RECOMP
        pixelShaderOutputs = pixelShader->outputs;
#endif
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR0)
            out += ",\n\tout float4 oC0 : SV_Target0";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR1)
            out += ",\n\tout float4 oC1 : SV_Target1";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR2)
            out += ",\n\tout float4 oC2 : SV_Target2";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR3)
            out += ",\n\tout float4 oC3 : SV_Target3";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_DEPTH)
            out += ",\n\tout float oDepth : SV_Depth";
    }
    else
    {
        auto vertexShader = reinterpret_cast<const VertexShader*>(shader);
#ifdef RENUT_RECOMP
        // A stream fetched by more than one vfetch gets one vertex element per
        // instruction, all naming the same semantic, so the declarations have
        // to be deduplicated even though the address map keeps every entry.
        std::unordered_set<uint32_t> declaredInputs;
#endif
        for (uint32_t i = 0; i < vertexShader->vertexElementCount; i++)
        {
            union
            {
                VertexElement vertexElement;
                uint32_t value;
            };

            value = vertexShader->vertexElementsAndInterpolators[vertexShader->field18 + i];

            vertexElements.emplace(uint32_t(vertexElement.address), vertexElement);

#ifdef RENUT_RECOMP
            if (!declaredInputs.insert((uint32_t(vertexElement.usage) << 8) | uint32_t(vertexElement.usageIndex)).second)
                continue;
#endif

            const char* usageType = USAGE_TYPES[uint32_t(vertexElement.usage)];

        #ifdef UNLEASHED_RECOMP
            if ((vertexElement.usage == DeclUsage::TexCoord && vertexElement.usageIndex == 2 && isMetaInstancer) ||
                (vertexElement.usage == DeclUsage::Position && vertexElement.usageIndex == 1))
            {
                usageType = "uint4";
            }
        #endif

#ifdef RENUT_RECOMP
            // A domain shader has no input assembler of its own: the
            // tessellator runs it once per generated vertex, and the control
            // points are read from the streams by patch index. The map of
            // addresses above is still filled, so the vfetches know their
            // semantics when they go through vfetchLive, but nothing is
            // declared as an attribute.
            if (isDomainShader)
                continue;
#endif

            out += '\t';

            bool locatedInput = false;
            for (auto& usageLocation : USAGE_LOCATIONS)
            {
                if (usageLocation.usage == vertexElement.usage && usageLocation.usageIndex == vertexElement.usageIndex)
                {
                    print("[[vk::location({})]] ", usageLocation.location);
                    locatedInput = true;
                    break;
                }
            }
#ifdef RENUT_RECOMP
            // Without a location the emitted input silently falls out of step
            // with the host's input layout, so refuse the shader instead.
            if (!locatedInput)
            {
                error("vertex element {}{} has no RENUT_VERTEX_INPUT_LOCATIONS entry",
                    USAGE_SEMANTICS[uint32_t(vertexElement.usage)], uint32_t(vertexElement.usageIndex));
            }
#else
            assert(locatedInput);
#endif

            println("in {0} i{1}{2} : {3}{2},", usageType, USAGE_VARIABLES[uint32_t(vertexElement.usage)],
                uint32_t(vertexElement.usageIndex), USAGE_SEMANTICS[uint32_t(vertexElement.usage)]);
        }

    #ifdef RENUT_RECOMP
        // The hardware loads the vertex index into r0.x before the shader runs.
        // A domain shader is run once per vertex of the expanded patch instead,
        // and the host names the point in the patch through a stream of its own
        // rather than through a tessellator the driver may not have.
        if (isDomainShader)
        {
            println("\t[[vk::location({})]] in float4 iDomainPatch : TEXCOORD31,",
                RENUT_DOMAIN_INPUT_LOCATION);
        }
        out += "\tin uint iVertexId : SV_VertexID,\n";
    #endif
    #ifdef UNLEASHED_RECOMP
        if (hasIndexCount)
        {
            out += "\tin uint iVertexId : SV_VertexID,\n";
            out += "\tin uint iInstanceId : SV_InstanceID,\n";
        }
    #endif

        out += "\tout float4 oPos : SV_Position";

        for (auto& [usage, usageIndex] : INTERPOLATORS)
            print(",\n\tout float4 o{0}{1} : {2}{1}", USAGE_VARIABLES[uint32_t(usage)], usageIndex, USAGE_SEMANTICS[uint32_t(usage)]);
    #ifdef RENUT_RECOMP
        // Declared last so it takes RENUT_POINT_SPRITE_LOCATION.
        out += ",\n\tout float4 oPointSprite : POINTSPRITE";
        // The guest's user clip planes (PA_CL_UCP_*), for the passes that ask
        // for them - N&B's quarter-resolution mirrored pass clips at a plane.
        out += ",\n\tout float oClipDistance : SV_ClipDistance0";
    #endif
    }

    out += ")\n";
    out += "{\n";

#ifdef UNLEASHED_RECOMP
    if (hasMtxProjection)
    {
        specConstantsMask |= SPEC_CONSTANT_REVERSE_Z;

        out += "\toPos = 0.0;\n";

        out += "\tfloat4x4 mtxProjection = float4x4(g_MtxProjection(0), g_MtxProjection(1), g_MtxProjection(2), g_MtxProjection(3));\n";
        out += "\tfloat4x4 mtxProjectionReverseZ = mul(mtxProjection, float4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 1, 1));\n";

        out += "\t[unroll] for (int iterationIndex = 0; iterationIndex < 2; iterationIndex++)\n";
        out += "\t{\n";
    }
#endif

    if (shaderContainer->definitionTableOffset != NULL)
    {
        auto definitionTable = reinterpret_cast<const DefinitionTable*>(shaderData + shaderContainer->definitionTableOffset);
        auto definitions = definitionTable->definitions;
        while (*definitions != 0)
        {
            auto definition = reinterpret_cast<const Float4Definition*>(definitions);
            auto value = reinterpret_cast<const be<uint32_t>*>(shaderData + shaderContainer->virtualSize + definition->physicalOffset);
            for (uint16_t i = 0; i < (definition->count + 3) / 4; i++)
            {
                // A pixel shader's definitions are indexed in the unified
                // 512-entry file, where its own half starts at 256.
                const uint32_t definedRegister = definition->registerIndex + i - (isPixelShader ? 256 : 0);
#ifdef RENUT_RECOMP
                literalConstants.insert(definedRegister);
#endif
                println("\tfloat4 c{} = asfloat(uint4(0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}));",
                    definedRegister, value[0].get(), value[1].get(), value[2].get(), value[3].get());

                value += 4;
            }
            definitions += 2;
        }
        ++definitions;
        while (*definitions != 0)
        {
            auto definition = reinterpret_cast<const Int4Definition*>(definitions);
            for (uint16_t i = 0; i < definition->count; i++)
            {
                union
                {
                    uint32_t value;
                    struct
                    {
                        int8_t x;
                        int8_t y;
                        int8_t z;
                        int8_t w;
                    };
                };

                value = definition->values[i].get();

                const uint32_t loopRegister = (definition->registerIndex - RENUT_LOOP_CONSTANT_REGISTER_BASE) / 4 + i;
#ifdef RENUT_RECOMP
                definedLoopConstants.insert(loopRegister);
#endif
                println("\tint4 i{} = int4({}, {}, {}, {});", loopRegister, x, y, z, w);
            }
            definitions += 2;
            definitions += definition->count;
        }

        out += "\n";
    }

    bool printedRegisters[GPR_COUNT]{};

    uint32_t interpolatorCount = (shader->interpolatorInfo >> 5) & 0x1F;

    for (uint32_t i = 0; i < interpolatorCount; i++)
    {
        union
        {
            Interpolator interpolator;
            uint32_t value;
        };

        if (isPixelShader)
        {
            value = reinterpret_cast<const PixelShader*>(shader)->interpolators[i];
#ifdef RENUT_RECOMP
            // Xenos links the stages by register: interpolator n is whatever the
            // vertex shader exported to register n, whatever either table calls
            // it. The UI 9-slice VS exports its UV as COLOR0 and the PS reads the
            // same register as TEXCOORD0; linked by name, the PS read zero.
            println("\tfloat4 r{0} = iTexCoord{0};", uint32_t(interpolator.reg));
#else
            println("\tfloat4 r{} = i{}{};", uint32_t(interpolator.reg), USAGE_VARIABLES[uint32_t(interpolator.usage)], uint32_t(interpolator.usageIndex));
#endif
            printedRegisters[interpolator.reg] = true;
        }
        else
        {
            auto vertexShader = reinterpret_cast<const VertexShader*>(shader);
            value = vertexShader->vertexElementsAndInterpolators[vertexShader->field18 + vertexShader->vertexElementCount + i];
#ifdef RENUT_RECOMP
            // By export register, as the pixel shader reads them (see above).
            if (interpolator.reg != i)
                std::fprintf(stderr, "vertex shader interpolator %u is tagged register %u\n", i, uint32_t(interpolator.reg));
            interpolators.emplace(i, fmt::format("oTexCoord{}", i));
#else
            interpolators.emplace(i, fmt::format("o{}{}", USAGE_VARIABLES[uint32_t(interpolator.usage)], uint32_t(interpolator.usageIndex)));
#endif
        }
    }

    if (!isPixelShader)
    {
    #if defined(UNLEASHED_RECOMP)
        if (!hasMtxProjection)
            out += "\toPos = 0.0;\n";
    #elif defined(RENUT_RECOMP)
        // Always define SV_Position so a skipped position-write block doesn't leave it undef.
        out += "\toPos = 0.0;\n";
    #endif

        for (auto& [usage, usageIndex] : INTERPOLATORS)
            println("\to{}{} = 0.0;", USAGE_VARIABLES[uint32_t(usage)], usageIndex);
    #ifdef RENUT_RECOMP
        out += "\toPointSprite = 0.0;\n";
        out += "\toClipDistance = 0.0;\n";
    #endif

        out += "\n";
    }

    for (size_t i = 0; i < GPR_COUNT; i++)
    {
        if (!printedRegisters[i])
        {
            print("\tfloat4 r{} = ", i);
            if (isPixelShader && i == ((shader->fieldC >> 8) & 0xFF))
            {
            #ifdef RENUT_RECOMP
                // The param-gen register. X is the pixel's x with back-facing in
                // the sign bit, Y its y with "is a point" in the sign bit, and
                // ZW the coordinate inside a point sprite - what the host's
                // point expansion hands over in iPointSprite, and zero for
                // anything else. A point is never back-facing.
                out += "float4((renutFragPos(iPos.xy) - 0.5) * float2((iPointSprite.x < 0.0) ? 1.0 : (iFace ? 1.0 : -1.0), "
                       "(iPointSprite.x < 0.0) ? -1.0 : 1.0), (iPointSprite.x < 0.0) ? saturate(iPointSprite.zw) : 0.0);\n";
            #else
                out += "float4((renutFragPos(iPos.xy) - 0.5) * float2(iFace ? 1.0 : -1.0, 1.0), 0.0, 0.0);\n";
            #endif
            }
        #ifdef RENUT_RECOMP
            // r0.xyz is the domain location in zyx order and r1.x the patch
            // index, the way the Xenos hands them to a patch-indexed triangle
            // domain shader.
            else if (isDomainShader && i == 0)
            {
                out += "float4(iDomainPatch.z, iDomainPatch.y, iDomainPatch.x, 0.0);\n";
            }
            else if (isDomainShader && i == 1)
            {
                out += "float4(iDomainPatch.w, 0.0, 0.0, 0.0);\n";
            }
            else if (!isPixelShader && i == 0)
            {
                out += "float4(float(iVertexId), 0.0, 0.0, 0.0);\n";
            }
        #endif
        #ifdef UNLEASHED_RECOMP
            else if (!isPixelShader && hasIndexCount && i == 0)
            {
                out += "float4(iVertexId + g_IndexCount.x * iInstanceId, 0.0, 0.0, 0.0);\n";
            }
        #endif
            else
            {
                out += "0.0;\n";
            }
        }
    }

    out += "\tint a0 = 0;\n";
    out += "\tint aL = 0;\n";
    out += "\tbool p0 = false;\n";
    out += "\tfloat ps = 0.0;\n";

#ifdef RENUT_RECOMP
    if (!isPixelShader)
    {
        // Point size (.x), edge flag (.y), kill vertex (.z). The size starts
        // at -1, which renutPointRadius reads as "not written" and replaces
        // with PA_SU_POINT_SIZE; the edge flag is not acted on.
        out += "\tfloat4 oPointSizeEdgeKill = float4(-1.0, 0.0, 0.0, 0.0);\n";
        // Memory export: the address, the five data registers, and which
        // of those have been written since the address last was.
        out += "\tfloat4 eA = 0.0;\n";
        out += "\tfloat4 eM0 = 0.0, eM1 = 0.0, eM2 = 0.0, eM3 = 0.0, eM4 = 0.0;\n";
        out += "\tuint eMWritten = 0;\n";
    }
#endif
    if (isPixelShader)
    {
#ifdef UNLEASHED_RECOMP
        out += "\tfloat2 pixelCoord = 0.0;\n";
#endif
        out += "\tCubeMapData cubeMapData = (CubeMapData)0;\n";
    }

    const be<uint32_t>* code = reinterpret_cast<const be<uint32_t>*>(shaderData + shaderContainer->virtualSize + shader->physicalOffset);

    union
    {
        ControlFlowInstruction controlFlow[2];
        struct
        {
            uint32_t code0;
            uint32_t code1;
            uint32_t code2;
            uint32_t code3;
        };
    };

    auto controlFlowCode = code;
    uint32_t instrAddress = 0;
    uint32_t instrSize = shader->size;
    bool simpleControlFlow = true;

    // Per-instruction control flow summary for the structurizer below.
    enum class CfKind : uint8_t { None, Cond, Uncond, LoopStart, LoopEnd };
    struct CfSummary
    {
        CfKind kind = CfKind::None;
        uint32_t target = 0;
    };
    std::vector<CfSummary> cfSummaries;
    std::vector<uint32_t> loopStarts;
    std::unordered_map<uint32_t, uint32_t> loopEndFor;
    uint32_t cfIndex = 0;

    while (instrAddress < instrSize)
    {
        code0 = controlFlowCode[0];
        code1 = controlFlowCode[1] & 0xFFFF;
        code2 = (controlFlowCode[1] >> 16) | (controlFlowCode[2] << 16);
        code3 = controlFlowCode[2] >> 16;

        for (auto& cfInstr : controlFlow)
        {
            uint32_t address = 0;

            switch (cfInstr.opcode)
            {
            case ControlFlowOpcode::Exec:
            case ControlFlowOpcode::ExecEnd:
                address = cfInstr.exec.address;
                break;

            case ControlFlowOpcode::CondExec:
            case ControlFlowOpcode::CondExecEnd:
            case ControlFlowOpcode::CondExecPredClean:
            case ControlFlowOpcode::CondExecPredCleanEnd:
                address = cfInstr.condExec.address;
                break;

            case ControlFlowOpcode::CondExecPred:
            case ControlFlowOpcode::CondExecPredEnd:
                address = cfInstr.condExecPred.address;
                break;

            case ControlFlowOpcode::LoopStart:
#ifdef RENUT_RECOMP
                loopConstants.insert(cfInstr.loopStart.loopId);
#endif
                loopStarts.push_back(cfIndex);
                cfSummaries.push_back({ CfKind::LoopStart, 0 });
                break;

            case ControlFlowOpcode::LoopEnd:
#ifdef RENUT_RECOMP
                loopConstants.insert(cfInstr.loopEnd.loopId);
#endif
                if (loopStarts.empty())
                    simpleControlFlow = false;
                else
                {
                    loopEndFor[loopStarts.back()] = cfIndex;
                    loopStarts.pop_back();
                }
                cfSummaries.push_back({ CfKind::LoopEnd, 0 });
                break;

            case ControlFlowOpcode::CondJmp:
            {
                if (cfInstr.condJmp.direction || cfInstr.condJmp.address <= cfIndex)
                    simpleControlFlow = false;

                cfSummaries.push_back({
                    cfInstr.condJmp.isUnconditional ? CfKind::Uncond : CfKind::Cond,
                    uint32_t(cfInstr.condJmp.address) });
                break;
            }
            }

            if (cfSummaries.size() == cfIndex)
                cfSummaries.push_back({});

            if (address != 0)
                instrSize = std::min<uint32_t>(instrSize, address * 12);

            ++cfIndex;
        }

        controlFlowCode += 3;
        instrAddress += 12;
    }

    if (!loopStarts.empty())
        simpleControlFlow = false;

    if (simpleControlFlow)
    {
        // Recursive-descent structurizer. Xenos control flow in practice is
        // structured code lowered to forward jumps: "cjmp c, ELSE; then;
        // jmp MERGE; ELSE: else" diamonds where the compiler may retarget the
        // then-exit jump at an ENCLOSING merge point (jump chaining), plus
        // LoopStart/LoopEnd pairs. structure() walks a region [lo, hi) with
        // the continuation index "cont" (where control flows after hi): a
        // trailing unconditional jump to the continuation is a no-op after
        // structuring, a conditional jump opens an if (with an else branch
        // when the instruction before its target is an unconditional jump to
        // a legal merge point). Any other shape rejects the shader back to
        // the switch(pc) state machine.
        const uint32_t instrCount = (instrSize / 12) * 2;
        if (cfSummaries.size() > instrCount)
            cfSummaries.resize(instrCount);

        std::function<bool(uint32_t, uint32_t, uint32_t)> structure =
            [&](uint32_t lo, uint32_t hi, uint32_t cont) -> bool
        {
            uint32_t i = lo;
            while (i < hi)
            {
                const CfSummary& summary = cfSummaries[i];
                switch (summary.kind)
                {
                case CfKind::Uncond:
                {
                    if (i + 1 == hi && (summary.target == cont || summary.target == hi))
                        i = hi;
                    else
                        return false;
                    break;
                }
                case CfKind::Cond:
                {
                    const uint32_t t = summary.target;
                    if (t > hi)
                    {
                        if (t != cont)
                            return false;
                        ++ifEndLabels[hi];
                        if (!structure(i + 1, hi, cont))
                            return false;
                        i = hi;
                    }
                    // A then-exit jump targeting the cond target itself is a
                    // no-op (empty else); fall through to the plain-if path,
                    // whose trailing-jump rule deletes it.
                    else if (t < hi && t - 1 > i && cfSummaries[t - 1].kind == CfKind::Uncond &&
                             cfSummaries[t - 1].target != t)
                    {
                        const uint32_t merge = cfSummaries[t - 1].target;
                        if (merge == cont || merge == hi)
                        {
                            elseLabels.insert(t);
                            ++ifEndLabels[hi];
                            if (!structure(i + 1, t - 1, merge == cont ? cont : hi))
                                return false;
                            if (!structure(t, hi, cont))
                                return false;
                            i = hi;
                        }
                        else if (merge < hi)
                        {
                            elseLabels.insert(t);
                            ++ifEndLabels[merge];
                            if (!structure(i + 1, t - 1, merge))
                                return false;
                            if (!structure(t, merge, merge))
                                return false;
                            i = merge;
                        }
                        else
                        {
                            return false;
                        }
                    }
                    else
                    {
                        ++ifEndLabels[t];
                        if (!structure(i + 1, t, t == hi ? cont : t))
                            return false;
                        i = t;
                    }
                    break;
                }
                case CfKind::LoopStart:
                {
                    auto loopEnd = loopEndFor.find(i);
                    if (loopEnd == loopEndFor.end() || loopEnd->second >= hi)
                        return false;
                    if (!structure(i + 1, loopEnd->second, loopEnd->second))
                        return false;
                    i = loopEnd->second + 1;
                    break;
                }
                case CfKind::LoopEnd:
                    return false; // reached without its LoopStart

                default:
                    ++i;
                    break;
                }
            }
            return true;
        };

        simpleControlFlow = structure(0, instrCount, instrCount);
    }

    if (!simpleControlFlow)
    {
        ifEndLabels.clear();
        elseLabels.clear();
    }

#ifdef RENUT_RECOMP
    // Emitted here rather than with the other locals because the slots a
    // shader loops on are only known once the control flow has been walked.
    for (uint32_t loopConstant : loopConstants)
    {
        if (definedLoopConstants.count(loopConstant) == 0)
            println("\tint4 i{0} = g_LoopConstant({0});", loopConstant);
    }
#endif

    if (simpleControlFlow)
    {
        out += '\n';
        indentation = 1;
    }
    else
    {
        out += "\n\tuint pc = 0;\n";
        out += "\twhile (true)\n";
        out += "\t{\n";
        out += "\t\tswitch (pc)\n";
        out += "\t\t{\n";
    }

    controlFlowCode = code;
    instrAddress = 0;
    uint32_t pc = 0;

    while (instrAddress < instrSize)
    {
        code0 = controlFlowCode[0];
        code1 = controlFlowCode[1] & 0xFFFF;
        code2 = (controlFlowCode[1] >> 16) | (controlFlowCode[2] << 16);
        code3 = controlFlowCode[2] >> 16;

        for (auto& cfInstr : controlFlow)
        {
            if (!simpleControlFlow)
            {
                indentation = 3;
                println("\t\tcase {}:", pc);
            }
            else
            {
                auto findResult = ifEndLabels.find(pc);
                if (findResult != ifEndLabels.end())
                {
                    for (uint32_t i = 0; i < findResult->second; i++)
                    {
                        --indentation;
                        indent();
                        out += "}\n";
                    }
                }
                // Inner regions close first (validated nesting), then the
                // enclosing if's then-branch flips to its else-branch.
                if (elseLabels.count(pc) != 0)
                {
                    --indentation;
                    indent();
                    out += "}\n";
                    indent();
                    out += "else\n";
                    indent();
                    out += "{\n";
                    ++indentation;
                }
            }

            ++pc;

            uint32_t address = 0;
            uint32_t count = 0;
            uint32_t sequence = 0;
            bool shouldReturn = false;
            bool shouldCloseCurlyBracket = false;

            switch (cfInstr.opcode)
            {
            case ControlFlowOpcode::Exec:
            case ControlFlowOpcode::ExecEnd:
                address = cfInstr.exec.address;
                count = cfInstr.exec.count;
                sequence = cfInstr.exec.sequence;
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::ExecEnd);
                break;

            case ControlFlowOpcode::CondExec:
            case ControlFlowOpcode::CondExecEnd:
            case ControlFlowOpcode::CondExecPredClean:
            case ControlFlowOpcode::CondExecPredCleanEnd:
                address = cfInstr.condExec.address;
                count = cfInstr.condExec.count;
                sequence = cfInstr.condExec.sequence;
#ifdef RENUT_RECOMP
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::CondExecEnd ||
                                cfInstr.opcode == ControlFlowOpcode::CondExecPredCleanEnd);
                // Gate the block on its boolean constant (Xenos cexec bN / cexec !bN); baseline ran it unconditionally.
                {
                    indent();
                    auto findResult = boolConstants.find(cfInstr.condExec.boolAddress);
                    if (findResult != boolConstants.end())
                        println("if ({}{})", cfInstr.condExec.condition ? "" : "!", findResult->second);
                    else
                        println("if ({}BOOL_BIT({}))", cfInstr.condExec.condition ? "" : "!", uint32_t(cfInstr.condExec.boolAddress));
                    indent();
                    out += "{\n";
                    ++indentation;
                    shouldCloseCurlyBracket = true;
                }
#else
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::CondExecEnd || cfInstr.opcode == ControlFlowOpcode::CondExecEnd);
#endif
                break;

            case ControlFlowOpcode::CondExecPred:
            case ControlFlowOpcode::CondExecPredEnd:
                address = cfInstr.condExecPred.address;
                count = cfInstr.condExecPred.count;
                sequence = cfInstr.condExecPred.sequence;
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::CondExecPredEnd);
#ifdef RENUT_RECOMP
                // The block runs only when p0 matches the condition bit. This
                // was being executed unconditionally, which N&B leans on hard:
                // 323 of these appear across the dump, guarding everything from
                // alpha cutouts to per-light branches.
                indent();
                println("if ({}p0)", cfInstr.condExecPred.condition ? "" : "!");
                indent();
                out += "{\n";
                ++indentation;
                shouldCloseCurlyBracket = true;
#endif
                break;

            case ControlFlowOpcode::LoopStart:
                if (simpleControlFlow)
                {
                #ifdef RENUT_RECOMP
                    // The loop constant is (count, initial aL, aL step), and
                    // N&B nests loops, so the iteration counter has to be local
                    // to this level - driving aL directly meant an inner loop
                    // trampled the enclosing one's index.
                    const uint32_t loopId = cfInstr.loopStart.loopId;
                    indent();
                    println("for (int aLi{0} = 0; aLi{0} < i{1}.x; aLi{0}++)", loopDepth, loopId);
                    indent();
                    out += "{\n";
                    ++indentation;
                    indent();
                    println("aL = i{}.y + i{}.z * aLi{};", loopId, loopId, loopDepth);
                    ++loopDepth;
                #else
                    indent();
                    #ifdef UNLEASHED_RECOMP
                    print("[unroll] ");
                    #endif
                    println("for (aL = 0; aL < i{}.x; aL++)", uint32_t(cfInstr.loopStart.loopId));
                    indent();
                    out += "{\n";
                    ++indentation;
                #endif
                }
                else
                {
                    out += "\t\t\taL = 0;\n";
                }
                break;

            case ControlFlowOpcode::LoopEnd:
                if (simpleControlFlow)
                {
                #ifdef RENUT_RECOMP
                    if (loopDepth != 0)
                        --loopDepth;
                #endif
                    --indentation;
                    indent();
                    out += "}\n";
                }
                else
                {
                    out += "\t\t\t++aL;\n";
                    println("\t\t\tif (aL < i{}.x)", uint32_t(cfInstr.loopEnd.loopId));
                    out += "\t\t\t{\n";
                    println("\t\t\t\tpc = {};", uint32_t(cfInstr.loopEnd.address));
                    out += "\t\t\t\tcontinue;\n";
                    out += "\t\t\t}\n";
                }
                break;

            case ControlFlowOpcode::CondJmp:
            {
                if (cfInstr.condJmp.isUnconditional)
                {
                    // Structured mode: the then-branch exit jump was consumed by
                    // the "} else {" emitted at the next instruction.
                    if (!simpleControlFlow)
                    {
                        println("\t\t\tpc = {};", uint32_t(cfInstr.condJmp.address));
                        out += "\t\t\tcontinue;\n";
                    }
                }
                else
                {
                    indent();
                    if (cfInstr.condJmp.isPredicated)
                    {
                        println("if ({}p0)", cfInstr.condJmp.condition ^ simpleControlFlow ? "" : "!");
                    }
                    else
                    {
#ifdef RENUT_RECOMP
                        // Bools expand to BOOL_BIT(N) expressions; negate when the jump fires on a clear bit.
                        const bool jumpIfSet = cfInstr.condJmp.condition ^ simpleControlFlow;
                        auto findResult = boolConstants.find(cfInstr.condJmp.boolAddress);
                        if (findResult != boolConstants.end())
                            println("if ({}{})", jumpIfSet ? "" : "!", findResult->second);
                        else
                            println("if ({}BOOL_BIT({}))", jumpIfSet ? "" : "!", uint32_t(cfInstr.condJmp.boolAddress));
#else
                        auto findResult = boolConstants.find(cfInstr.condJmp.boolAddress);
                        if (findResult != boolConstants.end())
                            println("if ((g_Booleans & {}) {}= 0)", findResult->second, cfInstr.condJmp.condition ^ simpleControlFlow ? "!" : "=");
                        else
                            println("if (b{} {}= 0)", uint32_t(cfInstr.condJmp.boolAddress), cfInstr.condJmp.condition ^ simpleControlFlow ? "!" : "=");
#endif
                    }

                    if (simpleControlFlow)
                    {
                        indent();
                        out += "{\n";
                        ++indentation;
                    }
                    else
                    {
                        out += "\t\t\t{\n";
                        println("\t\t\t\tpc = {};", uint32_t(cfInstr.condJmp.address));
                        out += "\t\t\t\tcontinue;\n";
                        out += "\t\t\t}\n";
                    }
                }
                break;
            }
            }

            auto instructionCode = code + address * 3;

            for (uint32_t i = 0; i < count; i++)
            {
                union
                {
                    VertexFetchInstruction vertexFetch;
                    TextureFetchInstruction textureFetch;
                    AluInstruction alu;
                    struct
                    {
                        uint32_t code0;
                        uint32_t code1;
                        uint32_t code2;
                    };
                };

                code0 = instructionCode[0];
                code1 = instructionCode[1];
                code2 = instructionCode[2];

                if ((sequence & 0x1) != 0)
                {
                    if (vertexFetch.opcode == FetchOpcode::VertexFetch)
                    {
                        recompile(vertexFetch, address + i);
                    }
                    else
                    {
                    #ifdef UNLEASHED_RECOMP
                        if (textureFetch.constIndex == 10) // g_GISampler
                        {
                            specConstantsMask |= SPEC_CONSTANT_BICUBIC_GI_FILTER;

                            indent();
                            out += "if (g_SpecConstants() & SPEC_CONSTANT_BICUBIC_GI_FILTER)";
                            indent();
                            out += '{';

                            ++indentation;
                            recompile(textureFetch, true);
                            --indentation;

                            indent();
                            out += "}";
                            indent();
                            out += "else";
                            indent();
                            out += '{';

                            ++indentation;
                            recompile(textureFetch, false);
                            --indentation;

                            indent();
                            out += '}';
                        }
                        else
                    #endif
                        {
                            recompile(textureFetch, false);
                        }
                    }
                }
                else
                {
                    recompile(alu);
                }

                sequence >>= 2;
                instructionCode += 3;
            }

            if (shouldReturn)
            {
                if (isPixelShader)
                {
                    specConstantsMask |= SPEC_CONSTANT_ALPHA_TEST;

                #ifdef RENUT_RECOMP
                    indent();
                    out += "[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)\n";
                    indent();
                    out += "{\n";
                    indent();
                    out += "\tclip(oC0.w - g_AlphaThreshold);\n";
                    indent();
                    out += "}\n";
                #else
                    indent();
                    out += "[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)";
                    indent();
                    out += '{';

                    indent();
                    out += "\tclip(oC0.w - g_AlphaThreshold);\n";

                    indent();
                    out += "}";
                #endif

                #ifdef UNLEASHED_RECOMP
                    specConstantsMask |= SPEC_CONSTANT_ALPHA_TO_COVERAGE;

                    indent();
                    out += "else if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TO_COVERAGE)";
                    indent();
                    out += '{';

                    indent();
                    out += "\toC0.w *= 1.0 + computeMipLevel(pixelCoord) * 0.25;\n";
                    indent();
                    out += "\toC0.w = 0.5 + (oC0.w - g_AlphaThreshold) / max(fwidth(oC0.w), 1e-6);\n";

                    indent();
                    out += '}';
                #endif
                }
                else
                {
                #ifdef RENUT_RECOMP
                    // Kill vertex: a non-zero .z means the vertex is discarded.
                    // Feeding SV_Position a NaN drops every primitive that uses
                    // it, which is the closest a raster pipeline gets.
                    // What the shader exported last is stored as it ends.
                    indent();
                    out += "renutMemExport(eA, eM0, eM1, eM2, eM3, eM4, eMWritten);\n";
                    indent();
                    out += "if (oPointSizeEdgeKill.z != 0.0)\n";
                    indent();
                    out += "\toPos = asfloat(0x7FC00000u);\n";
                #endif

                #ifdef UNLEASHED_RECOMP
                    if (!hasMtxProjection)
                #endif
                    {
                    #ifdef RENUT_RECOMP
                        // The scale is what makes a viewport-disabled draw work.
                        // With D3DRS_VIEWPORTENABLE clear the hardware skips the
                        // viewport transform, so the guest shader emits window
                        // coordinates; a host raster pipeline would cull all but
                        // the 1x1 corner of the clip volume. The host supplies
                        // (2/w, 2/h, -1, -1) for those draws and an identity
                        // (1, 1, 0, 0) for every normal one, so this costs a
                        // multiply-add and changes nothing in the common case.
                        out += "\toClipDistance = renutUserClipDistance(oPos);\n";
                        out += "\toPos.xy = oPos.xy * g_ViewportScaleOffset.xy + "
                               "(g_ViewportScaleOffset.zw + g_HalfPixelOffset) * oPos.w;\n";
                        // Only a point-list draw reads this, in the host's
                        // expansion into a sprite; see RENUT_POINT_SPRITE_LOCATION.
                        out += "\toPointSprite = float4(renutPointRadius(oPointSizeEdgeKill.x), 0.0, 0.0);\n";
                    #else
                        out += "\toPos.xy += g_HalfPixelOffset * oPos.w;\n";
                    #endif
                    }
                }

                if (simpleControlFlow)
                {
                    indent();
                #ifdef UNLEASHED_RECOMP
                    if (hasMtxProjection)
                    {
                        out += "continue;\n";
                    }
                    else
                #endif
                    {
                        out += "return;\n";
                    }
                }
                else
                {
                    out += "\t\t\tbreak;\n";
                }
            }

            if (shouldCloseCurlyBracket)
            {
                --indentation;
                indent();
                out += "}\n";
            }
        }

        controlFlowCode += 3;
        instrAddress += 12;
    }

    if (!simpleControlFlow)
    {
        out += "\t\t\tbreak;\n";
        out += "\t\t}\n";
        out += "\t\tbreak;\n";
        out += "\t}\n";
    }
    else
    {
        // Regions closing one past the last instruction never get visited by
        // the loop above; balance their braces here.
        auto findResult = ifEndLabels.find(pc);
        if (findResult != ifEndLabels.end())
        {
            for (uint32_t i = 0; i < findResult->second; i++)
            {
                --indentation;
                indent();
                out += "}\n";
            }
        }
    }

#ifdef UNLEASHED_RECOMP
    if (hasMtxProjection)
        out += "\t}\n";

    if (!isPixelShader && hasMtxProjection)
        out += "\toPos.xy += g_HalfPixelOffset * oPos.w;\n";
#endif

#ifdef RENUT_RECOMP
    // The float constants the export addresses are built from, so the host
    // looks for the streams a draw exports into there and nowhere else: the
    // register file keeps the stream constants of earlier draws too. Stored
    // as the range [first, last] in the flags (RENUT_SHADER_FLAG_MEMEXPORT).
    if (specConstantsMask & RENUT_SHADER_FLAG_MEMEXPORT)
    {
        uint32_t first = 255, last = 0;
        size_t at = out.find("void main(");
        while (at != std::string::npos && (at = out.find("\teA.", at)) != std::string::npos)
        {
            size_t end = out.find('\n', at);
            if (end == std::string::npos)
                end = out.size();
            for (size_t c = out.find("cReg", at); c != std::string::npos && c < end; c = out.find("cReg", c + 4))
            {
                // cReg( or cRegRel( - a relative read counts by its base index.
                const char* arg = out.c_str() + c + 4;
                if (std::strncmp(arg, "Rel", 3) == 0)
                    arg += 3;
                if (*arg != '(')
                    continue;
                uint32_t index = uint32_t(std::strtoul(arg + 1, nullptr, 10));
                first = std::min(first, index);
                last = std::max(last, index);
            }
            at = end;
        }
        if (first > last)
        {
            first = 0;
            last = 255;
        }
        specConstantsMask |= (first << 8) | (last << 16);
    }
#endif

    out += "}";
}
