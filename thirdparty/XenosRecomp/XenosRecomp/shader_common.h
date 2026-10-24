#ifndef SHADER_COMMON_H_INCLUDED
#define SHADER_COMMON_H_INCLUDED

#define SPEC_CONSTANT_R11G11B10_NORMAL  (1 << 0)
#define SPEC_CONSTANT_ALPHA_TEST        (1 << 1)

#ifdef UNLEASHED_RECOMP
    #define SPEC_CONSTANT_BICUBIC_GI_FILTER (1 << 2)
    #define SPEC_CONSTANT_ALPHA_TO_COVERAGE (1 << 3)
    #define SPEC_CONSTANT_REVERSE_Z         (1 << 4)
#endif

// ---------------------------------------------------------------------------
// Register file sizes.
//
// Nuts & Bolts ships almost all of its shaders as bare microcode: 239 of the
// 256 blobs the game hands to CreateVertexShader/CreatePixelShader carry no
// D3DX constant table at all, so nothing names the constants, samplers or
// boolean registers a shader body reads. Every one of these files is therefore
// addressed positionally, exactly the way the hardware does it, and the sizes
// below are the hardware's - not whatever a particular shader happened to
// declare.
// ---------------------------------------------------------------------------

// The texture fetch constant index in a Xenos fetch instruction is 5 bits, so
// any of 32 slots is reachable. N&B pixel shaders really do reach slot 16
// (ps_3735d7c453e42101 and friends), which is past the 16 the Unleashed/BD
// layout allowed for.
#define RENUT_TEXTURE_SLOT_COUNT 32

// 256 float4 constants per stage. The hardware file is 512 entries split
// VS 0..255 / PS 256..511, but shader microcode addresses its own half with an
// 8-bit index, so both stages see 0..255. (The 224 the PC ps_3_0 model allows
// is too small: N&B pixel shaders address c224..c255.)
#define RENUT_SHADER_CONSTANT_COUNT 256

// 32 loop/int4 constants in one file, VS 0..15 and PS 16..31. These are the
// indices a LoopStart's loopId and the shader's own int4 definition table use
// (a definition's register index is 8992 + 4 * slot).
#define RENUT_LOOP_CONSTANT_COUNT 32
#define RENUT_LOOP_CONSTANT_REGISTER_BASE 8992

// 256 boolean registers in one file, VS 0..127 and PS 128..255. N&B control
// flow tests bools 0..3 from vertex shaders and 128..134 from pixel shaders.
#define RENUT_BOOL_REGISTER_COUNT 256

// ---------------------------------------------------------------------------
// SharedConstants layout, in bytes. Both the SPIR-V (raw buffer) and the DXIL
// (cbuffer) paths below describe this same block, so a host only ever fills it
// in once.
// ---------------------------------------------------------------------------
#define RENUT_SHARED_TEXTURE2D_OFFSET    0                                                   //    0 .. 127
#define RENUT_SHARED_TEXTURE3D_OFFSET    (RENUT_TEXTURE_SLOT_COUNT * 4)                      //  128 .. 255
#define RENUT_SHARED_TEXTURECUBE_OFFSET  (RENUT_TEXTURE_SLOT_COUNT * 8)                      //  256 .. 383
#define RENUT_SHARED_SAMPLER_OFFSET      (RENUT_TEXTURE_SLOT_COUNT * 12)                     //  384 .. 511
#define RENUT_SHARED_BOOLEANS_OFFSET     (RENUT_TEXTURE_SLOT_COUNT * 16)                     //  512 .. 543
#define RENUT_SHARED_MISC_OFFSET         (RENUT_SHARED_BOOLEANS_OFFSET + RENUT_BOOL_REGISTER_COUNT / 8)   //  544 ..
// Misc runs to +40, then 8 bytes of padding so the float4 at +48 lands on a
// 16-byte boundary (544 + 48 = 592), which is what a float4 load wants.
// Bytes 40..47 are unused.
// Predicated tiling done once by the host: the row where the lower band
// starts, 0 when the draw is not merged (float, in the padding at +40).
#define RENUT_SHARED_BAND_SPLIT_OFFSET   (RENUT_SHARED_MISC_OFFSET + 40)                     //  584
#define RENUT_SHARED_VIEWPORT_OFFSET     (RENUT_SHARED_MISC_OFFSET + 48)                     //  592

// Device addresses of the sixteen bound vertex streams, eight bytes each.
//
// A vfetch does not have to be a vertex input. The hardware indexes a stream
// with whatever register the instruction names, and when that index is not the
// vertex id there is no input attribute that can express it - N&B's skinned
// meshes are the case that matters, fetching bone matrices out of stream 10 by
// blend index, three float4 rows at a 64-byte stride. Those reads become raw
// buffer loads against the addresses here.
//
// Sixteen because that is how many streams SetStreamSource can bind: it writes
// vertex fetch constant 95 - StreamNumber, and a shader names fetch constant
// constIndex * 3 + constIndexSelect, so the two meet in this table.
//
// Entry 16 is vertex fetch constant 0, which Direct3D's own shaders (the
// rectangle-list Clear) fetch through; entry 17 pads the table to 16 bytes.
// A vertex shader the game also runs as the domain stage of a tessellated
// draw is recompiled a second time and stored under the container's hash xored
// with this, so one cache holds both forms. See RENUT_STAGE_DOMAIN.
#define RENUT_DOMAIN_SHADER_HASH_TAG 0xD0A1D0A1D0A1D0A1ull

#define RENUT_VERTEX_STREAM_COUNT 17
#define RENUT_DIRECT_FETCH_STREAM 16
#define RENUT_SHARED_STREAMS_OFFSET      (RENUT_SHARED_MISC_OFFSET + 64)                     //  608 .. 751
// Per-draw vertex input flags that did not fit in the misc block.
#define RENUT_SHARED_INPUTS_OFFSET       (RENUT_SHARED_STREAMS_OFFSET + 18 * 8)              //  752 .. 767
// Everything above changes from one draw to the next - the textures, the
// booleans, the streams - and everything below hardly ever does, so the two
// halves are separate uniform buffers (bindings 8 and 9 of set 4): a draw
// uploads the 768 bytes of the first instead of the whole block.
#define RENUT_SHARED_HOT_SIZE            (RENUT_SHARED_INPUTS_OFFSET + 16)                   //  768
// The loop constants, int4 each (count, start, step).
#define RENUT_SHARED_LOOP_OFFSET         RENUT_SHARED_HOT_SIZE                               //  768 .. 1279
// Live vertex fetches (see vfetchLive): uint4 per fetch - stream | format << 8
// | flags << 16, stride bytes, offset bytes, exp scale as float bits.
#define RENUT_LIVE_FETCH_COUNT           32
#define RENUT_LIVE_FETCH_LOCATION        0xFE
// A live fetch's second word carries its stride in the low 16 bits and, in
// bits 16..27, the component remap of RENUT_SHARED_REMAP_OFFSET for it.
#define RENUT_SHARED_FETCH_OFFSET        (RENUT_SHARED_LOOP_OFFSET + RENUT_LOOP_CONSTANT_COUNT * 16)  // 1280 .. 1791
// Point sprites. A float4 in guest pixels - PA_SU_POINT_SIZE's width and
// height, the diameter used when the vertex shader writes none, then
// PA_SU_POINT_MINMAX's min and max, which clamp the one it writes - and a
// float2 that turns a diameter in guest pixels into a radius in host NDC. The
// second is signed like the viewport, so -y is always the top of the screen.
#define RENUT_SHARED_POINT_OFFSET        (RENUT_SHARED_FETCH_OFFSET + RENUT_LIVE_FETCH_COUNT * 16)   // 1792 .. 1823
// Memory export. The guest memory a vertex shader may write this draw, as up
// to eight ranges of uint4 (guest physical base in bytes, size in bytes,
// device address low, high). The host keeps each exported range in a buffer
// of its own and hands later draws that fetch from it that buffer instead of
// guest memory; a store outside every range is dropped. See renutMemExport.
#define RENUT_EXPORT_RANGE_COUNT         8
#define RENUT_SHARED_EXPORT_OFFSET       (RENUT_SHARED_POINT_OFFSET + 32)                    // 1824 .. 1951
// Vertex input component remap, a uint per input location: which fetched
// component (0..3), or the constant 0 (4) or 1 (5), each of the four
// components becomes, three bits each, stored xored with the identity (0x688)
// so that zero changes nothing. It turns what the recompiled vfetch reads into
// what the live one does - see ShaderInputElement::dstSwizzle.
#define RENUT_INPUT_REMAP_COUNT          32
#define RENUT_INPUT_REMAP_IDENTITY       0x688u
// ...and handed to the shaders as specialization constants, id 16 + location,
// for the locations the vertex shaders can have (0..23). See g_SpecInputRemap0.
#define RENUT_SPEC_INPUT_REMAP_BASE      16
#define RENUT_SPEC_INPUT_REMAP_LOCATIONS 24
// The boolean registers, a word of 32 each: id RENUT_SPEC_BOOLS_BASE + word.
#define RENUT_SPEC_BOOLS_BASE            40
#define RENUT_SHARED_REMAP_OFFSET        (RENUT_SHARED_EXPORT_OFFSET + RENUT_EXPORT_RANGE_COUNT * 16)  // 1952 .. 2079
// User clip planes: PA_CL_UCP_0..5 in the guest's clip space, then the mask
// of enabled ones (PA_CL_CLIP_CNTL.ucp_ena, zero under clip_disable). A vertex
// shader turns them into SV_ClipDistance0 - see renutUserClipDistance.
#define RENUT_USER_CLIP_PLANE_COUNT      6
#define RENUT_SHARED_UCP_OFFSET          (RENUT_SHARED_REMAP_OFFSET + RENUT_INPUT_REMAP_COUNT * 4)     // 2080 .. 2175
#define RENUT_SHARED_UCP_MASK_OFFSET     (RENUT_SHARED_UCP_OFFSET + RENUT_USER_CLIP_PLANE_COUNT * 16)  // 2176
#define RENUT_SHARED_SIZE                (RENUT_SHARED_UCP_MASK_OFFSET + 16)                           // 2192

// Set in a cache entry's specConstantsMask when the shader exports to memory,
// so the host knows to look for the streams it writes. Not a specialization
// constant. Bits 8..15 and 16..23 then hold the first and last float constant
// the export addresses are built from.
#define RENUT_SHADER_FLAG_MEMEXPORT      (1u << 31)
// Set when a vertex shader reads the float constants by a0 or aL: the host then
// places the whole file as one block for g_SpecRelConstants (spec id 12, the
// mode 0..2 of g_VertexShaderConstantRel). Not a specialization constant.
#define RENUT_SHADER_FLAG_REL_CONSTANTS  (1u << 30)
#define RENUT_SPEC_REL_CONSTANTS         12
#define RENUT_SHADER_EXPORT_FIRST(mask)  (((mask) >> 8) & 0xFF)
#define RENUT_SHADER_EXPORT_LAST(mask)   (((mask) >> 16) & 0xFF)

// The interpolator location after RENUT_INTERPOLATORS that carries a point
// sprite from the vertex shader through the host's point expansion to the
// pixel shader. The vertex shader writes the sprite's radius in NDC to .xy;
// the expansion replaces the whole value with (-1, 0, s, t), and the pixel
// shader reads a negative .x as "this is a point" and .zw as the coordinate
// inside it, the way the Xenos hands them over in its param-gen register.
#define RENUT_POINT_SPRITE_LOCATION      23

// SPIR-V vertex input locations, shared with the host input-layout builder. A
// semantic missing here gets no location from the emitter, and the recompiler
// treats that as a hard error rather than emitting an unlocated input.
//
// N&B binds POSITION up to index 4, TEXCOORD up to 10, COLOR up to 1, and uses
// NORMAL/TANGENT/BINORMAL at index 0. BLENDWEIGHT/BLENDINDICES appear in no
// shader in the current dump but cost nothing to reserve.
//
// TEXCOORD5..10 are appended out of order on purpose. Every location here is
// baked into the recompiled SPIR-V and matched against the host input layout
// built from this same list, so the two only agree if they were generated
// together; renumbering the middle of the table would be silently wrong against
// any shader cache that was not regenerated in the same breath. Appending keeps
// every existing location where it was.
//
// The first four indices were all the 256-shader dump ever used. The full dump
// - 1169 shaders, once XGRegisterVertexShader was hooked - reaches TEXCOORD10.
#define RENUT_VERTEX_INPUT_LOCATIONS(X) \
    X(Position,     0,  0) \
    X(Position,     1,  1) \
    X(Position,     2,  2) \
    X(Position,     3,  3) \
    X(Position,     4,  4) \
    X(Normal,       0,  5) \
    X(Tangent,      0,  6) \
    X(Binormal,     0,  7) \
    X(TexCoord,     0,  8) \
    X(TexCoord,     1,  9) \
    X(TexCoord,     2, 10) \
    X(TexCoord,     3, 11) \
    X(TexCoord,     4, 12) \
    X(Color,        0, 13) \
    X(Color,        1, 14) \
    X(BlendWeight,  0, 15) \
    X(BlendIndices, 0, 16) \
    X(TexCoord,     5, 17) \
    X(TexCoord,     6, 18) \
    X(TexCoord,     7, 19) \
    X(TexCoord,     8, 20) \
    X(TexCoord,     9, 21) \
    X(TexCoord,    10, 22)

// Where a domain shader reads the coordinates of the point in the patch it is
// running for: xyz the barycentric weights, w the patch index. The host fills
// this stream when it expands a patch draw. See RENUT_DOMAIN_SHADER_HASH_TAG.
#define RENUT_DOMAIN_INPUT_LOCATION 23

#ifdef __cplusplus
#include <cstdint>

// One vertex input a shader fetches, as the shader itself describes it.
//
// N&B mostly does not call D3DDevice_SetVertexDeclaration - 338 of 340 level
// draws in a run arrive with nothing bound. That is not an oversight on its
// part: on Xenos the fetch layout belongs to the *shader*, whose own vertex
// element table names the semantics and whose vfetch instructions carry the
// format, the offset, the stride and which stream to read. A D3D vertex
// declaration is one way to describe that and the game only bothers for the
// handful of draws that need to override it.
//
// So the recompiler exports what it already had to parse, and a host with no
// declaration in hand builds its input layout out of this instead.
//
// `format` is the six-bit Xenos vertex fetch format. `isSigned` and
// `isNormalized` come from the instruction's formatCompAll and numFormatAll
// bits and are what tell an 8_8_8_8 apart as UNORM, SNORM or UINT.
struct ShaderInputElement
{
    uint8_t usage;          // DeclUsage
    uint8_t usageIndex;
    uint8_t location;       // from RENUT_VERTEX_INPUT_LOCATIONS
    uint8_t slot;           // vertex stream, 95 - fetch constant

    uint16_t offsetBytes;
    uint16_t strideBytes;

    uint8_t format;
    uint8_t isSigned;
    uint8_t isNormalized;
    uint8_t padding;

    // Instruction slot of the vfetch that reads this element. Direct3D
    // rewrites a vertex shader's vfetch instructions (stream, format, stride,
    // offset) when it binds the shader to a vertex declaration, so the fields
    // above only describe the shader as registered; a host that sees the
    // bound microcode decodes the live values from this instruction.
    uint16_t fetchAddress;
    // The destination swizzle of that vfetch as this shader was recompiled
    // with it, baked into the code. Direct3D rewrites it too - .yxwz for a
    // 16-bit element, .zyxw for a D3DCOLOR, 0 and 1 for the components a
    // declaration does not have - and a shader found by its loose key was
    // compiled from microcode that predates the rewrite, so the host compares
    // the two and remaps the fetched components (RENUT_SHARED_REMAP_OFFSET).
    uint16_t dstSwizzle;
};
#endif

// Every (usage, usageIndex) pair a vertex shader may export and a pixel shader
// may read. Both stages declare the whole set so any VS pairs with any PS.
// N&B interpolates normals, tangents and binormals directly rather than
// packing them into spare TEXCOORDs, so those semantics have to be here.
#define RENUT_INTERPOLATORS(X) \
    X(TexCoord,  0) \
    X(TexCoord,  1) \
    X(TexCoord,  2) \
    X(TexCoord,  3) \
    X(TexCoord,  4) \
    X(TexCoord,  5) \
    X(TexCoord,  6) \
    X(TexCoord,  7) \
    X(TexCoord,  8) \
    X(TexCoord,  9) \
    X(TexCoord, 10) \
    X(TexCoord, 11) \
    X(TexCoord, 12) \
    X(TexCoord, 13) \
    X(TexCoord, 14) \
    X(TexCoord, 15) \
    X(Color,     0) \
    X(Color,     1) \
    X(Normal,    0) \
    X(Normal,    1) \
    X(Normal,    2) \
    X(Tangent,   0) \
    X(Binormal,  0)

// SPEC_CONSTANTS_ONLY keeps the HLSL below out of host C++ TUs, which IntelliSense would otherwise parse.
#if (!defined(__cplusplus) || defined(__INTELLISENSE__)) && !defined(SHADER_COMMON_SPEC_CONSTANTS_ONLY)

#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)

#ifdef __spirv__

struct PushConstants
{
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

#ifdef RENUT_RECOMP

// The two stages' float4 constant files and the shared block, as uniform
// buffers in set 4 that the host binds with a dynamic offset per draw. They
// used to be raw buffer loads from the addresses in the push constants, and
// on Adreno that is the slow way to read a constant: every one became a global
// load in the shader - hoisted into a preamble each draw waits on, or repeated
// in every wave - where a uniform buffer read at a constant offset is
// promoted into the constant file, which the command processor fills before
// the draw starts. Measured on Turnip, that load latency was most of what a
// draw of the guest's shaders cost the GPU.
//
// Each stage's file is split into RENUT_CONSTANT_CHUNK_COUNT blocks of
// RENUT_CONSTANT_CHUNK_SIZE float4s, bound separately, so that a draw that
// changes a few registers - its matrices, nearly always - uploads the block
// holding them and keeps the others where they are.
#define RENUT_CONSTANT_CHUNK_SIZE  64
#define RENUT_CONSTANT_CHUNK_COUNT 4
[[vk::binding(0, 4)]] cbuffer RenutVertexConstants0 { float4 g_VertexConstants0[RENUT_CONSTANT_CHUNK_SIZE]; };
[[vk::binding(1, 4)]] cbuffer RenutVertexConstants1 { float4 g_VertexConstants1[RENUT_CONSTANT_CHUNK_SIZE]; };
[[vk::binding(2, 4)]] cbuffer RenutVertexConstants2 { float4 g_VertexConstants2[RENUT_CONSTANT_CHUNK_SIZE]; };
[[vk::binding(3, 4)]] cbuffer RenutVertexConstants3 { float4 g_VertexConstants3[RENUT_CONSTANT_CHUNK_SIZE]; };
[[vk::binding(4, 4)]] cbuffer RenutPixelConstants0 { float4 g_PixelConstants0[RENUT_CONSTANT_CHUNK_SIZE]; };
[[vk::binding(5, 4)]] cbuffer RenutPixelConstants1 { float4 g_PixelConstants1[RENUT_CONSTANT_CHUNK_SIZE]; };
[[vk::binding(6, 4)]] cbuffer RenutPixelConstants2 { float4 g_PixelConstants2[RENUT_CONSTANT_CHUNK_SIZE]; };
[[vk::binding(7, 4)]] cbuffer RenutPixelConstants3 { float4 g_PixelConstants3[RENUT_CONSTANT_CHUNK_SIZE]; };
[[vk::binding(8, 4)]] cbuffer RenutSharedConstants
{
    uint4 g_SharedWords[RENUT_SHARED_HOT_SIZE / 16];
};
[[vk::binding(9, 4)]] cbuffer RenutSharedColdConstants
{
    uint4 g_SharedColdWords[(RENUT_SHARED_SIZE - RENUT_SHARED_HOT_SIZE) / 16];
};

// A constant index known when the shader is compiled folds to one block and
// offset; a relative one (a0, aL) picks the block at run time.
float4 g_VertexShaderConstant(uint index)
{
    index = min(index, uint(RENUT_SHADER_CONSTANT_COUNT - 1));
    const uint i = index % RENUT_CONSTANT_CHUNK_SIZE;
    switch (index / RENUT_CONSTANT_CHUNK_SIZE)
    {
    case 0: return g_VertexConstants0[i];
    case 1: return g_VertexConstants1[i];
    case 2: return g_VertexConstants2[i];
    default: return g_VertexConstants3[i];
    }
}
// Relative reads of the vertex file (a0, aL: a skinned mesh's bone matrices)
// can land in any of the four blocks, so Turnip promotes all four whole into the
// constant file on every draw and the shader picks one with a select per read.
// With g_SpecRelConstants set the host also binds the whole file as one block
// (binding 10, 256 float4s, at the offset it placed the draw's blocks) and a
// relative read is a single read of it. Mode 2 reads the same bytes through
// binding 11, declared far larger than the file so that the driver cannot
// promote it into the constant file and loads each read instead. The indices
// are clamped to the file in every mode, so the extra range is never read.
#define RENUT_REL_LARGE_COUNT 4096
[[vk::binding(10, 4)]] cbuffer RenutVertexConstantsAll { float4 g_VertexConstantsAll[RENUT_SHADER_CONSTANT_COUNT]; };
[[vk::binding(11, 4)]] cbuffer RenutVertexConstantsAllLarge { float4 g_VertexConstantsAllLarge[RENUT_REL_LARGE_COUNT]; };
[[vk::constant_id(12)]] const uint g_SpecRelConstants = 0;
float4 g_VertexShaderConstantRel(uint index)
{
    index = min(index, uint(RENUT_SHADER_CONSTANT_COUNT - 1));
    if (g_SpecRelConstants == 1)
        return g_VertexConstantsAll[index];
    if (g_SpecRelConstants == 2)
        return g_VertexConstantsAllLarge[index];
    return g_VertexShaderConstant(index);
}
float4 g_PixelShaderConstant(uint index)
{
    index = min(index, uint(RENUT_SHADER_CONSTANT_COUNT - 1));
    const uint i = index % RENUT_CONSTANT_CHUNK_SIZE;
    switch (index / RENUT_CONSTANT_CHUNK_SIZE)
    {
    case 0: return g_PixelConstants0[i];
    case 1: return g_PixelConstants1[i];
    case 2: return g_PixelConstants2[i];
    default: return g_PixelConstants3[i];
    }
}
float4 g_PixelShaderConstantRel(uint index) { return g_PixelShaderConstant(index); }

// The shared block by byte offset, in the RENUT_SHARED_* layout above.
// Either half by the offset in the whole layout. An offset known when the
// shader compiles picks its half there; both indices are clamped so that one
// that is not stays inside both arrays whichever the select throws away.
#define renutSharedU4(OFFSET)  ((OFFSET) < RENUT_SHARED_HOT_SIZE \
    ? g_SharedWords[min(uint(OFFSET) >> 4, RENUT_SHARED_HOT_SIZE / 16 - 1)] \
    : g_SharedColdWords[min((uint(OFFSET) - RENUT_SHARED_HOT_SIZE) >> 4, \
                            (RENUT_SHARED_SIZE - RENUT_SHARED_HOT_SIZE) / 16 - 1)])
#define renutSharedU(OFFSET)   (renutSharedU4(OFFSET)[((OFFSET) >> 2) & 3])
#define renutSharedF(OFFSET)   asfloat(renutSharedU(OFFSET))
#define renutSharedF2(OFFSET)  float2(renutSharedF(OFFSET), renutSharedF((OFFSET) + 4))
#define renutSharedF4(OFFSET)  asfloat(renutSharedU4(OFFSET))
// The pixel position the shader reads (param gen). The host draws the upper
// band of the game's predicated tiling over the whole screen and moves the
// game's own lower-band draws below it; on the console each band starts at
// row 0, and shaders that turn the position into texture coordinates (water
// reflections and refraction) expect that. Below the split the position is
// given back relative to the lower band.
#define renutFragPos(P) float2((P).x, (P).y - ((renutSharedF(RENUT_SHARED_BAND_SPLIT_OFFSET) > 0.0 && \
    (P).y >= renutSharedF(RENUT_SHARED_BAND_SPLIT_OFFSET)) ? renutSharedF(RENUT_SHARED_BAND_SPLIT_OFFSET) : 0.0))
#define renutSharedU64(OFFSET) (uint64_t(renutSharedU(OFFSET)) | (uint64_t(renutSharedU((OFFSET) + 4)) << 32))

#define texture2DDescriptorIndex(SLOT) \
    renutSharedU(RENUT_SHARED_TEXTURE2D_OFFSET + (SLOT) * 4)
#define texture3DDescriptorIndex(SLOT) \
    renutSharedU(RENUT_SHARED_TEXTURE3D_OFFSET + (SLOT) * 4)
#define textureCubeDescriptorIndex(SLOT) \
    renutSharedU(RENUT_SHARED_TEXTURECUBE_OFFSET + (SLOT) * 4)
#define samplerDescriptorIndex(SLOT) \
    renutSharedU(RENUT_SHARED_SAMPLER_OFFSET + (SLOT) * 4)

#define g_Booleans(i)         renutSharedU(RENUT_SHARED_BOOLEANS_OFFSET + (i) * 4)
#define g_LoopConstant(i)     asint(renutSharedU4(RENUT_SHARED_LOOP_OFFSET + (i) * 16))

#define g_HalfPixelOffset     renutSharedF2(RENUT_SHARED_MISC_OFFSET + 4)
#define g_AlphaThreshold      renutSharedF(RENUT_SHARED_MISC_OFFSET + 12)

// How the host bound each vertex input, and which user clip planes are on, as
// specialization constants. Every one of them follows from the pipeline's own
// vertex input layout (or, for the clip mask, a register that changes a few
// times a frame), so the host keys its pipelines on them anyway - and fixed at
// pipeline creation the driver folds each test away. Read from the shared
// block instead they were uniform, and Turnip computed all of them in the
// shader's preamble, which runs once per draw before any of its waves start:
// a vertex shader's median preamble was 414 instructions. The shared block
// keeps their old offsets unused so that its layout does not move.
[[vk::constant_id(1)]] const uint g_SpecPackedInputs = 0;
[[vk::constant_id(2)]] const uint g_SpecIntInputs = 0;
[[vk::constant_id(3)]] const uint g_SpecUserClipMask = 0;
[[vk::constant_id(4)]] const uint g_SpecSwappedTexcoords = 0;
[[vk::constant_id(5)]] const uint g_SpecSwappedNormals = 0;
[[vk::constant_id(6)]] const uint g_SpecSwappedBinormals = 0;
[[vk::constant_id(7)]] const uint g_SpecSwappedTangents = 0;
[[vk::constant_id(8)]] const uint g_SpecSwappedBlendWeights = 0;
[[vk::constant_id(9)]] const uint g_SpecSwappedPositions = 0;
[[vk::constant_id(10)]] const uint g_SpecSwappedBlendIndices = 0;
[[vk::constant_id(11)]] const uint g_SpecSintTexcoords = 0;
// The input remap of each vertex input location, 16 + location (see
// RENUT_SHARED_REMAP_OFFSET for the encoding). Locations stop at 23.
[[vk::constant_id(16)]] const uint g_SpecInputRemap0 = 0;
[[vk::constant_id(17)]] const uint g_SpecInputRemap1 = 0;
[[vk::constant_id(18)]] const uint g_SpecInputRemap2 = 0;
[[vk::constant_id(19)]] const uint g_SpecInputRemap3 = 0;
[[vk::constant_id(20)]] const uint g_SpecInputRemap4 = 0;
[[vk::constant_id(21)]] const uint g_SpecInputRemap5 = 0;
[[vk::constant_id(22)]] const uint g_SpecInputRemap6 = 0;
[[vk::constant_id(23)]] const uint g_SpecInputRemap7 = 0;
[[vk::constant_id(24)]] const uint g_SpecInputRemap8 = 0;
[[vk::constant_id(25)]] const uint g_SpecInputRemap9 = 0;
[[vk::constant_id(26)]] const uint g_SpecInputRemap10 = 0;
[[vk::constant_id(27)]] const uint g_SpecInputRemap11 = 0;
[[vk::constant_id(28)]] const uint g_SpecInputRemap12 = 0;
[[vk::constant_id(29)]] const uint g_SpecInputRemap13 = 0;
[[vk::constant_id(30)]] const uint g_SpecInputRemap14 = 0;
[[vk::constant_id(31)]] const uint g_SpecInputRemap15 = 0;
[[vk::constant_id(32)]] const uint g_SpecInputRemap16 = 0;
[[vk::constant_id(33)]] const uint g_SpecInputRemap17 = 0;
[[vk::constant_id(34)]] const uint g_SpecInputRemap18 = 0;
[[vk::constant_id(35)]] const uint g_SpecInputRemap19 = 0;
[[vk::constant_id(36)]] const uint g_SpecInputRemap20 = 0;
[[vk::constant_id(37)]] const uint g_SpecInputRemap21 = 0;
[[vk::constant_id(38)]] const uint g_SpecInputRemap22 = 0;
[[vk::constant_id(39)]] const uint g_SpecInputRemap23 = 0;

// The boolean registers, as specialization constants too: id
// RENUT_SPEC_BOOLS_BASE + word, bit n % 32 of word n / 32. The host keys each
// pipeline on the bits its two shaders test (and leaves the rest zero), so the
// driver drops the branches a draw does not take. The lit material tests
// sixteen of them for eight chained point lights and its shadow cascades; read
// as uniforms, every draw carried all of that code, ~1,100 instructions.
[[vk::constant_id(40)]] const uint g_SpecBools0 = 0;
[[vk::constant_id(41)]] const uint g_SpecBools1 = 0;
[[vk::constant_id(42)]] const uint g_SpecBools2 = 0;
[[vk::constant_id(43)]] const uint g_SpecBools3 = 0;
[[vk::constant_id(44)]] const uint g_SpecBools4 = 0;
[[vk::constant_id(45)]] const uint g_SpecBools5 = 0;
[[vk::constant_id(46)]] const uint g_SpecBools6 = 0;
[[vk::constant_id(47)]] const uint g_SpecBools7 = 0;
#define g_SpecBoolWord(w) ((w) == 0 ? g_SpecBools0 : (w) == 1 ? g_SpecBools1 :                            (w) == 2 ? g_SpecBools2 : (w) == 3 ? g_SpecBools3 :                            (w) == 4 ? g_SpecBools4 : (w) == 5 ? g_SpecBools5 :                            (w) == 6 ? g_SpecBools6 : g_SpecBools7)

#define g_SwappedTexcoords    g_SpecSwappedTexcoords
#define g_SwappedNormals      g_SpecSwappedNormals
#define g_SwappedBinormals    g_SpecSwappedBinormals
#define g_SwappedTangents     g_SpecSwappedTangents
#define g_SwappedBlendWeights g_SpecSwappedBlendWeights
#define g_SwappedPositions    g_SpecSwappedPositions
#define g_SintTexcoords       g_SpecSintTexcoords
#define g_SwappedBlendIndices g_SpecSwappedBlendIndices
#define g_PackedInputs        g_SpecPackedInputs
#define g_IntInputs           g_SpecIntInputs

// xy scale, zw offset, applied to the vertex position on its way to SV_Position.
//
// Normally (1, 1, 0, 0), because the guest shader already emits clip space. It
// is not identity when the guest has cleared D3DRS_VIEWPORTENABLE, which tells
// the hardware to skip the viewport transform - the shader then emits *window*
// coordinates, and without a scale everything outside the 1x1 corner of the
// clip volume is culled. N&B's FMV renderer does exactly that: it hands the
// pipeline a pixel-space rectangle and disables the viewport.
#define g_ViewportScaleOffset renutSharedF4(RENUT_SHARED_VIEWPORT_OFFSET)

#define g_PointSize           renutSharedF4(RENUT_SHARED_POINT_OFFSET)
#define g_PointScreenToNdc    renutSharedF2(RENUT_SHARED_POINT_OFFSET + 16)

// The distance to the nearest enabled user clip plane, from the position the
// guest shader wrote (clip space, before g_ViewportScaleOffset), as xenia does.
// Clipping keeps what is >= 0 on every plane, which is the minimum of them; a
// draw with no plane enabled gets a distance that clips nothing.
float renutUserClipDistance(float4 position)
{
    uint mask = g_SpecUserClipMask;
    float distance = 3.0e38;
    [unroll] for (uint i = 0; i < RENUT_USER_CLIP_PLANE_COUNT; ++i)
    {
        if (mask & (1u << i))
            distance = min(distance, dot(position,
                renutSharedF4(RENUT_SHARED_UCP_OFFSET + i * 16)));
    }
    return distance;
}

// A vfetch the vertex declaration does not describe - see the stream table's
// comment. `index` is whatever register the instruction named, so it arrives as
// a float holding an integer; `strideBytes` and `offsetBytes` are the
// instruction's own, converted from the dwords it stores them in.
//
// A stream the host left null reads as zero rather than faulting, which is what
// a draw that binds fewer streams than the shader fetches would otherwise do.
float4 vfetchStream(uint stream, float index, uint strideBytes, uint offsetBytes)
{
    uint64_t address = renutSharedU64(RENUT_SHARED_STREAMS_OFFSET + stream * 8);

    if (address == 0)
        return 0.0;

    return vk::RawBufferLoad<float4>(address + uint(index) * strideBytes + offsetBytes, 0x10);
}

// The same fetch for every other Xenos vertex format.
//
// The host uploads streams swapped per 32-bit word (k_8in32), so each dword
// reads back as the guest's own value and the component layout is the one the
// hardware documents: X in the low bits. `flags` packs the instruction's
// format_comp_all (bit 0, signed), num_format_all (bit 1, integer rather than
// fraction) and signed_rf_mode_all (bit 2, full range); `expScale` is
// 2^exp_adjust. Components the format lacks read as (0, 0, 0, 1), and the
// destination swizzle picks what it needs from that.
float vfetchNormalize(uint raw, uint bits, uint flags)
{
    bool isSigned = (flags & 1) != 0;
    bool isInteger = (flags & 2) != 0;
    bool isFullRange = (flags & 4) != 0;

    if (isSigned)
    {
        int value = int(raw << (32 - bits)) >> (32 - bits);
        if (isInteger)
            return float(value);
        float maxValue = float((1u << (bits - 1)) - 1);
        if (isFullRange)
            return (float(value) * 2.0 + 1.0) / (maxValue * 2.0 + 1.0);
        return max(float(value) / maxValue, -1.0);
    }

    if (isInteger)
        return float(raw);
    return float(raw) / float((1u << bits) - 1);
}

float4 vfetchStreamFormat(uint stream, float index, uint strideBytes, uint offsetBytes,
    uint format, uint flags, float expScale)
{
    uint64_t address = renutSharedU64(RENUT_SHARED_STREAMS_OFFSET + stream * 8);

    if (address == 0)
        return 0.0;

    address += uint(index) * strideBytes + offsetBytes;

    float4 result = float4(0.0, 0.0, 0.0, 1.0);
    bool isFloat = false;

    switch (format)
    {
    case 6: // k_8_8_8_8
    {
        uint w = vk::RawBufferLoad<uint>(address, 4);
        result = float4(vfetchNormalize(w & 0xFF, 8, flags), vfetchNormalize((w >> 8) & 0xFF, 8, flags),
            vfetchNormalize((w >> 16) & 0xFF, 8, flags), vfetchNormalize(w >> 24, 8, flags));
        break;
    }
    case 7: // k_2_10_10_10
    {
        uint w = vk::RawBufferLoad<uint>(address, 4);
        result = float4(vfetchNormalize(w & 0x3FF, 10, flags), vfetchNormalize((w >> 10) & 0x3FF, 10, flags),
            vfetchNormalize((w >> 20) & 0x3FF, 10, flags), vfetchNormalize(w >> 30, 2, flags));
        break;
    }
    case 16: // k_10_11_11
    {
        uint w = vk::RawBufferLoad<uint>(address, 4);
        result.xyz = float3(vfetchNormalize(w & 0x7FF, 11, flags), vfetchNormalize((w >> 11) & 0x7FF, 11, flags),
            vfetchNormalize(w >> 22, 10, flags));
        break;
    }
    case 17: // k_11_11_10
    {
        uint w = vk::RawBufferLoad<uint>(address, 4);
        result.xyz = float3(vfetchNormalize(w & 0x3FF, 10, flags), vfetchNormalize((w >> 10) & 0x7FF, 11, flags),
            vfetchNormalize(w >> 21, 11, flags));
        break;
    }
    case 25: // k_16_16
    {
        uint w = vk::RawBufferLoad<uint>(address, 4);
        result.xy = float2(vfetchNormalize(w & 0xFFFF, 16, flags), vfetchNormalize(w >> 16, 16, flags));
        break;
    }
    case 26: // k_16_16_16_16
    {
        uint2 w = vk::RawBufferLoad<uint2>(address, 4);
        result = float4(vfetchNormalize(w.x & 0xFFFF, 16, flags), vfetchNormalize(w.x >> 16, 16, flags),
            vfetchNormalize(w.y & 0xFFFF, 16, flags), vfetchNormalize(w.y >> 16, 16, flags));
        break;
    }
    case 31: // k_16_16_FLOAT
    {
        uint w = vk::RawBufferLoad<uint>(address, 4);
        result.xy = f16tof32(uint2(w & 0xFFFF, w >> 16));
        isFloat = true;
        break;
    }
    case 32: // k_16_16_16_16_FLOAT
    {
        uint2 w = vk::RawBufferLoad<uint2>(address, 4);
        result = f16tof32(uint4(w.x & 0xFFFF, w.x >> 16, w.y & 0xFFFF, w.y >> 16));
        isFloat = true;
        break;
    }
    case 33: // k_32
        result.x = vfetchNormalize(vk::RawBufferLoad<uint>(address, 4), 32, flags | 2);
        break;
    case 34: // k_32_32
    {
        uint2 w = vk::RawBufferLoad<uint2>(address, 4);
        result.xy = float2(vfetchNormalize(w.x, 32, flags | 2), vfetchNormalize(w.y, 32, flags | 2));
        break;
    }
    case 35: // k_32_32_32_32
    {
        uint4 w = vk::RawBufferLoad<uint4>(address, 4);
        result = float4(vfetchNormalize(w.x, 32, flags | 2), vfetchNormalize(w.y, 32, flags | 2),
            vfetchNormalize(w.z, 32, flags | 2), vfetchNormalize(w.w, 32, flags | 2));
        break;
    }
    case 36: // k_32_FLOAT
        result.x = vk::RawBufferLoad<float>(address, 4);
        isFloat = true;
        break;
    case 37: // k_32_32_FLOAT
        result.xy = vk::RawBufferLoad<float2>(address, 4);
        isFloat = true;
        break;
    case 38: // k_32_32_32_32_FLOAT
        result = vk::RawBufferLoad<float4>(address, 4);
        isFloat = true;
        break;
    case 57: // k_32_32_32_FLOAT
        result.xyz = vk::RawBufferLoad<float3>(address, 4);
        isFloat = true;
        break;
    }

    // exp_adjust scales fixed-point results only; floats pass through.
    if (!isFloat)
        result *= expScale;

    return result;
}

// A vertex input indexed by a register the shader computed, with the stream,
// format, stride and offset of the live (bind-time patched) instruction.
float4 renutRemap(float4 value, uint code);

float4 vfetchLive(uint k, float index)
{
    uint4 d = renutSharedU4(RENUT_SHARED_FETCH_OFFSET + k * 16);
    float4 value = vfetchStreamFormat(d.x & 0xFF, index, d.y & 0xFFFF, d.z, (d.x >> 8) & 0xFF, (d.x >> 16) & 0xFF, asfloat(d.w));
    return renutRemap(value, (d.y >> 16) & 0xFFF);
}

// Per-stage float4 constant file. The recompiler picks the stage by defining
// RENUT_STAGE_CONSTANTS to the matching stage's lookup.
#define cReg(INDEX) RENUT_STAGE_CONSTANTS(uint(INDEX))
// A relative read (a0, aL): see g_VertexShaderConstantRel.
#define cRegRel(INDEX) RENUT_STAGE_REL_CONSTANTS(uint(INDEX))

#else
#define g_Booleans                 renutSharedU(256)
#define g_SwappedTexcoords         renutSharedU(260)
#define g_HalfPixelOffset          vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 264)
#define g_AlphaThreshold           vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 272)
#endif

[[vk::constant_id(0)]] const uint g_SpecConstants = 0;

#define g_SpecConstants() g_SpecConstants

#else

#ifdef RENUT_RECOMP
// Mirrors the byte offsets above; c<n> == byte n * 16.
#define DEFINE_SHARED_CONSTANTS() \
    uint4  g_Texture2DDescriptorIndices[8]   : packoffset(c0); \
    uint4  g_Texture3DDescriptorIndices[8]   : packoffset(c8); \
    uint4  g_TextureCubeDescriptorIndices[8] : packoffset(c16); \
    uint4  g_SamplerDescriptorIndices[8]     : packoffset(c24); \
    uint4  g_BooleansArr[2]                  : packoffset(c32); \
    int4   g_LoopConstants[32]               : packoffset(c48); \
    uint   g_SwappedTexcoords                : packoffset(c34.x); \
    float2 g_HalfPixelOffset                 : packoffset(c34.y); \
    float  g_AlphaThreshold                  : packoffset(c34.w); \
    uint   g_SwappedNormals                  : packoffset(c35.x); \
    uint   g_SwappedBinormals                : packoffset(c35.y); \
    uint   g_SwappedTangents                 : packoffset(c35.z); \
    uint   g_SwappedBlendWeights             : packoffset(c35.w); \
    uint   g_SwappedPositions                : packoffset(c36.x); \
    uint   g_SintTexcoords                   : packoffset(c36.y); \
    uint   g_SwappedBlendIndices             : packoffset(c36.z); \
    uint   g_PackedInputs                    : packoffset(c36.w); \
    float4 g_ViewportScaleOffset             : packoffset(c37); \
    uint   g_IntInputs                       : packoffset(c47.x); \
    float4 g_PointSize                       : packoffset(c112); \
    float2 g_PointScreenToNdc                : packoffset(c113);

#define texture2DDescriptorIndex(SLOT)   (g_Texture2DDescriptorIndices[(SLOT) / 4][(SLOT) % 4])
#define texture3DDescriptorIndex(SLOT)   (g_Texture3DDescriptorIndices[(SLOT) / 4][(SLOT) % 4])
#define textureCubeDescriptorIndex(SLOT) (g_TextureCubeDescriptorIndices[(SLOT) / 4][(SLOT) % 4])
#define samplerDescriptorIndex(SLOT)     (g_SamplerDescriptorIndices[(SLOT) / 4][(SLOT) % 4])

#define g_Booleans(i)     (g_BooleansArr[(i) / 4][(i) % 4])
#define g_LoopConstant(i) (g_LoopConstants[i])

// The recompiler declares g_ShaderConstants in the per-stage cbuffer.
#define cReg(INDEX) g_ShaderConstants[min(uint(INDEX), uint(RENUT_SHADER_CONSTANT_COUNT - 1))]
#define cRegRel(INDEX) cReg(INDEX)

#else
#define DEFINE_SHARED_CONSTANTS() \
    uint g_Booleans : packoffset(c16.x); \
    uint g_SwappedTexcoords : packoffset(c16.y); \
    float2 g_HalfPixelOffset : packoffset(c16.z); \
    float g_AlphaThreshold : packoffset(c17.x);
#endif

uint g_SpecConstants();

#endif

#ifndef renutFragPos
#define renutFragPos(P) (P)
#endif

#ifdef RENUT_RECOMP
// Test Xenos boolean register N in the unified VS(0..127)/PS(128..255) file.
#ifdef __spirv__
#define BOOL_BIT(n) ((g_SpecBoolWord((n) / 32u) & (1u << ((n) & 31u))) != 0)
#else
#define BOOL_BIT(n) ((g_Booleans((n) / 32u) & (1u << ((n) & 31u))) != 0)
#endif

// The radius in host NDC of the sprite a point becomes. `size` is the diameter
// the vertex shader exported in guest pixels, or -1 if it exported none; as on
// the hardware, one it did export is clamped to PA_SU_POINT_MINMAX.
float2 renutPointRadius(float size)
{
    float4 sizes = g_PointSize;
    float2 diameter = size >= 0.0 ? clamp(size, sizes.z, sizes.w).xx : sizes.xy;
    return diameter * g_PointScreenToNdc;
}

// Memory export, as the Xenos does it. eA is a stream constant with the index
// added in: X the base in dwords (with 01 in the top bits), Y the element
// index, Z the format (bits 8..13), signedness (16), integer rather than
// normalized (17), red/blue swap (19) and endianness (0..2), W the stream's
// element count - Y, Z and W as integers in the mantissa of 2^23. eM0..eM4 are
// consecutive elements from that index; `written` says which were set. The
// host keeps exported memory the way it keeps every vertex stream: guest
// memory with each dword byte-swapped (k8in32). So an element is packed as the
// hardware packs it - X in the low bits - and each dword then goes through the
// export's own endian swap and the host's k8in32 one. The two cancel for a
// k8in32 export; a k8in16 one comes out with its halves exchanged, which is
// what the game relies on when it reads back a stream it exported that way.
// The readers see exactly the dwords the hardware's would: their Direct3D
// fetch swizzle (.yxwz for 16-bit elements) is put back by the host, see
// ShaderInputElement::dstSwizzle. Formats narrower than a dword are not
// stored.
#ifdef __spirv__
// The export's endian swap followed by k8in32.
uint renutExportEndian(uint value, uint endian)
{
    switch (endian)
    {
    case 0:  // none
        return (value >> 24) | ((value >> 8) & 0xFF00) | ((value << 8) & 0xFF0000) | (value << 24);
    case 1:  // k8in16
        return (value >> 16) | (value << 16);
    case 3:  // k16in32
        return ((value >> 8) & 0x00FF00FF) | ((value & 0x00FF00FF) << 8);
    default: // k8in32, and the 64/128-bit forms taken as it
        return value;
    }
}

void renutExportDword(uint address, uint value, uint endian)
{
    value = renutExportEndian(value, endian);
    for (uint i = 0; i < RENUT_EXPORT_RANGE_COUNT; ++i)
    {
        uint4 range = renutSharedU4(RENUT_SHARED_EXPORT_OFFSET + i * 16);
        if (address - range.x < range.y)
        {
            uint64_t device = (uint64_t(range.w) << 32) | uint64_t(range.z);
            vk::RawBufferStore<uint>(device + (address - range.x), value, 4);
            return;
        }
    }
}

// A float to half precision, rounded to nearest even. f32tof16 becomes
// packHalf2x16, whose rounding is the driver's choice, and Turnip truncates:
// state a shader reads, updates and exports again every frame - the falling
// leaves' rotation, whose length is also their size - then shrinks by up to
// 2^-11 a frame, to a third of itself in a minute and a half.
uint renutHalf(float value)
{
    uint bits = asuint(value);
    uint sign = (bits >> 16) & 0x8000;
    uint magnitude = bits & 0x7FFFFFFF;
    if (magnitude >= 0x47800000)  // 65536 and up, infinity, NaN
        return sign | (magnitude > 0x7F800000 ? 0x7E00 : 0x7C00);
    if (magnitude < 0x38800000)   // below 2^-14: a subnormal half, or zero
        return sign | uint(round(asfloat(magnitude) * 16777216.0));
    uint result = (magnitude >> 13) - ((127 - 15) << 10);
    uint rest = magnitude & 0x1FFF;
    if (rest > 0x1000 || (rest == 0x1000 && (result & 1) != 0))
        ++result;
    return sign | result;
}

// Packs up to four fixed-point fields, X in the low bits, by the Direct3D
// conversion rules: clamped, scaled if normalized, rounded half away from
// zero. A zero width ends the list.
uint renutExportFixed(float4 value, uint4 widths, bool isSigned, bool isNorm)
{
    uint packed = 0;
    uint offset = 0;
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        uint width = widths[i];
        if (width != 0)
        {
            float c = isnan(value[i]) ? 0.0 : value[i];
            uint field;
            if (isSigned)
            {
                float maxValue = float((1u << (width - 1)) - 1);
                c = isNorm ? clamp(c, -1.0, 1.0) * maxValue : clamp(c, -maxValue - 1.0, maxValue);
                field = uint(int(c + (c < 0.0 ? -0.5 : 0.5))) & ((1u << width) - 1);
            }
            else
            {
                float maxValue = float((1u << width) - 1);
                c = isNorm ? saturate(c) * maxValue : clamp(c, 0.0, maxValue);
                field = uint(c + 0.5);
            }
            packed |= field << offset;
            offset += width;
        }
    }
    return packed;
}

void renutMemExport(float4 eA, float4 eM0, float4 eM1, float4 eM2, float4 eM3, float4 eM4, uint written)
{
    uint4 a = asuint(eA);
    if (written == 0 || any((a >> uint4(30, 23, 23, 23)) != uint4(1, 0x96, 0x96, 0x96)))
        return;
    uint info = a.z;
    uint format = (info >> 8) & 0x3F;
    bool isSigned = (info & (1u << 16)) != 0;
    bool isNorm = (info & (1u << 17)) == 0;
    bool swapRedBlue = (info & (1u << 19)) != 0;
    uint index = a.y & 0x7FFFFF;
    uint count = a.w & 0x7FFFFF;
    uint base = a.x << 2;
    uint endian = info & 7;

    // kind: 0 fixed point in one dword, 1 16_16_16_16, 2 16_16_FLOAT,
    // 3 16_16_16_16_FLOAT, 4.. 32-bit floats, one to four.
    uint kind;
    uint bytesLog2;
    uint4 widths = 0;
    switch (format)
    {
    case 6: case 14: case 50:  kind = 0; bytesLog2 = 2; widths = uint4(8, 8, 8, 8);    break;  // 8_8_8_8
    case 7: case 54:           kind = 0; bytesLog2 = 2; widths = uint4(10, 10, 10, 2); break;  // 2_10_10_10
    case 16: case 55:          kind = 0; bytesLog2 = 2; widths = uint4(11, 11, 10, 0); break;  // 10_11_11
    case 17: case 56:          kind = 0; bytesLog2 = 2; widths = uint4(10, 11, 11, 0); break;  // 11_11_10
    case 25:                   kind = 0; bytesLog2 = 2; widths = uint4(16, 16, 0, 0);  break;  // 16_16
    case 26:                   kind = 1; bytesLog2 = 3; widths = uint4(16, 16, 0, 0);  break;  // 16_16_16_16
    case 31:                   kind = 2; bytesLog2 = 2; break;  // 16_16_FLOAT
    case 32:                   kind = 3; bytesLog2 = 3; break;  // 16_16_16_16_FLOAT
    case 36:                   kind = 4; bytesLog2 = 2; break;  // 32_FLOAT
    case 37:                   kind = 5; bytesLog2 = 3; break;  // 32_32_FLOAT
    case 38:                   kind = 6; bytesLog2 = 4; break;  // 32_32_32_32_FLOAT
    default:
        return;
    }

    float4 eM[5] = { eM0, eM1, eM2, eM3, eM4 };
    [unroll] for (uint k = 0; k < 5; ++k)
    {
        if (((written >> k) & 1) == 0 || index + k >= count)
            continue;
        float4 v = swapRedBlue ? eM[k].zyxw : eM[k];
        uint address = base + ((index + k) << bytesLog2);
        switch (kind)
        {
        case 0:
            renutExportDword(address, renutExportFixed(v, widths, isSigned, isNorm), endian);
            break;
        case 1:
            renutExportDword(address, renutExportFixed(v.xyxy, widths, isSigned, isNorm), endian);
            renutExportDword(address + 4, renutExportFixed(v.zwzw, widths, isSigned, isNorm), endian);
            break;
        case 2:
            renutExportDword(address, renutHalf(v.x) | (renutHalf(v.y) << 16), endian);
            break;
        case 3:
            renutExportDword(address, renutHalf(v.x) | (renutHalf(v.y) << 16), endian);
            renutExportDword(address + 4, renutHalf(v.z) | (renutHalf(v.w) << 16), endian);
            break;
        default:
            renutExportDword(address, asuint(v.x), endian);
            if (kind >= 5)
                renutExportDword(address + 4, asuint(v.y), endian);
            if (kind >= 6)
            {
                renutExportDword(address + 8, asuint(v.z), endian);
                renutExportDword(address + 12, asuint(v.w), endian);
            }
            break;
        }
    }
}
#else
void renutMemExport(float4 eA, float4 eM0, float4 eM1, float4 eM2, float4 eM3, float4 eM4, uint written)
{
}
#endif
#endif

#ifdef RENUT_RECOMP
// A vertex stage has no implicit derivatives, so every fetch there has to name
// its level explicitly. N&B does sample from vertex shaders - 1D ramps and 3D
// volumes - which a derivative-based Sample() cannot express outside a pixel
// or compute stage. The recompiler defines RENUT_STAGE_VERTEX ahead of this
// header so the helpers below compile for whichever stage they land in.
#ifdef RENUT_STAGE_VERTEX
#define RENUT_SAMPLE(TEX, SAMP, COORD) (TEX).SampleLevel(SAMP, COORD, 0.0)
#else
#define RENUT_SAMPLE(TEX, SAMP, COORD) (TEX).Sample(SAMP, COORD)
#endif
#else
#define RENUT_SAMPLE(TEX, SAMP, COORD) (TEX).Sample(SAMP, COORD)
#endif

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
Texture3D<float4> g_Texture3DDescriptorHeap[] : register(t0, space1);
TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);

uint2 getTexture2DDimensions(Texture2D<float4> texture)
{
    uint2 dimensions;
    texture.GetDimensions(dimensions.x, dimensions.y);
    return dimensions;
}

float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    // 93% of the game's 2D fetches have no offset, and the division by the
    // texture's size - a size query per sample - cannot fold away on its own:
    // 0 / 0 is NaN. With a literal zero offset this branch is compiled out.
    float2 coord = texCoord;
    if (any(offset != 0.0))
        coord += offset / getTexture2DDimensions(texture);
    return RENUT_SAMPLE(texture, g_SamplerDescriptorHeap[samplerDescriptorIndex], coord);
}

float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return select(isnan(texCoord), 0.0, frac(texCoord * getTexture2DDimensions(texture) + offset - 0.5));
}

#ifdef RENUT_RECOMP
// A Xenos 1D texture is a 2D surface one texel tall, and the host binds it in
// the Texture2D heap, so a 1D fetch is a 2D fetch down the middle of row 0.
// N&B uses this for the vertex-shader ramp lookup in vs_d38d10b70f954f86.
float4 tfetch1D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float texCoord, float2 offset)
{
    return tfetch2D(resourceDescriptorIndex, samplerDescriptorIndex, float2(texCoord, 0.5), offset);
}

float2 getWeights1D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float texCoord, float2 offset)
{
    return getWeights2D(resourceDescriptorIndex, samplerDescriptorIndex, float2(texCoord, 0.5), offset);
}
#endif

float w0(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-a + 3.0f) - 3.0f) + 1.0f);
}

float w1(float a)
{
    return (1.0f / 6.0f) * (a * a * (3.0f * a - 6.0f) + 4.0f);
}

float w2(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-3.0f * a + 3.0f) + 3.0f) + 1.0f);
}

float w3(float a)
{
    return (1.0f / 6.0f) * (a * a * a);
}

float g0(float a)
{
    return w0(a) + w1(a);
}

float g1(float a)
{
    return w2(a) + w3(a);
}

float h0(float a)
{
    return -1.0f + w1(a) / (w0(a) + w1(a)) + 0.5f;
}

float h1(float a)
{
    return 1.0f + w3(a) / (w2(a) + w3(a)) + 0.5f;
}

float4 tfetch2DBicubic(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    SamplerState samplerState = g_SamplerDescriptorHeap[samplerDescriptorIndex];
    uint2 dimensions = getTexture2DDimensions(texture);

    float x = texCoord.x * dimensions.x + offset.x;
    float y = texCoord.y * dimensions.y + offset.y;

    x -= 0.5f;
    y -= 0.5f;
    float px = floor(x);
    float py = floor(y);
    float fx = x - px;
    float fy = y - py;

    float g0x = g0(fx);
    float g1x = g1(fx);
    float h0x = h0(fx);
    float h1x = h1(fx);
    float h0y = h0(fy);
    float h1y = h1(fy);

    float4 r =
        g0(fy) * (g0x * RENUT_SAMPLE(texture, samplerState, float2(px + h0x, py + h0y) / float2(dimensions)) +
            g1x * RENUT_SAMPLE(texture, samplerState, float2(px + h1x, py + h0y) / float2(dimensions))) +
        g1(fy) * (g0x * RENUT_SAMPLE(texture, samplerState, float2(px + h0x, py + h1y) / float2(dimensions)) +
            g1x * RENUT_SAMPLE(texture, samplerState, float2(px + h1x, py + h1y) / float2(dimensions)));

    return r;
}

float4 tfetch3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord)
{
    return RENUT_SAMPLE(g_Texture3DDescriptorHeap[resourceDescriptorIndex], g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord);
}

struct CubeMapData
{
	#ifdef RENUT_RECOMP
    // No N&B shader in the dump issues a cube instruction, but the slot count
    // has to cover the most any one shader could stack up before its fetches.
    float3 cubeMapDirections[4];
    #else
    float3 cubeMapDirections[2];
    #endif
    uint cubeMapIndex;
};

float4 tfetchCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData)
{
    return RENUT_SAMPLE(g_TextureCubeDescriptorHeap[resourceDescriptorIndex], g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z]);
}

#ifdef RENUT_RECOMP
// DEC3N normal decode; IA binds as R32_UINT so lane .x carries the raw bits (asuint recovers them).
float4 tfetchR11G11B10(float4 value)
{
    if (g_SpecConstants() & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        uint v = asuint(value.x);
        return float4(
            (v & 0x00000400 ? -1.0 : 0.0) + ((v & 0x3FF) / 1024.0),
            (v & 0x00200000 ? -1.0 : 0.0) + (((v >> 11) & 0x3FF) / 1024.0),
            (v & 0x80000000 ? -1.0 : 0.0) + (((v >> 22) & 0x1FF) / 512.0),
            0.0);
    }
    return value;
}

// The fetched components as the live vfetch has them; `code` is a remap
// stored xored with the identity (see RENUT_SHARED_REMAP_OFFSET).
float4 renutRemap(float4 value, uint code)
{
    if (code == 0)
        return value;
    code ^= RENUT_INPUT_REMAP_IDENTITY;
    float4 result;
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        uint source = (code >> (3 * i)) & 7;
        result[i] = source < 4 ? value[source] : (source == 5 ? 1.0 : 0.0);
    }
    return result;
}

#ifdef __spirv__
uint renutInputRemap(uint location)
{
    switch (location)
    {
    case 0: return g_SpecInputRemap0;
    case 1: return g_SpecInputRemap1;
    case 2: return g_SpecInputRemap2;
    case 3: return g_SpecInputRemap3;
    case 4: return g_SpecInputRemap4;
    case 5: return g_SpecInputRemap5;
    case 6: return g_SpecInputRemap6;
    case 7: return g_SpecInputRemap7;
    case 8: return g_SpecInputRemap8;
    case 9: return g_SpecInputRemap9;
    case 10: return g_SpecInputRemap10;
    case 11: return g_SpecInputRemap11;
    case 12: return g_SpecInputRemap12;
    case 13: return g_SpecInputRemap13;
    case 14: return g_SpecInputRemap14;
    case 15: return g_SpecInputRemap15;
    case 16: return g_SpecInputRemap16;
    case 17: return g_SpecInputRemap17;
    case 18: return g_SpecInputRemap18;
    case 19: return g_SpecInputRemap19;
    case 20: return g_SpecInputRemap20;
    case 21: return g_SpecInputRemap21;
    case 22: return g_SpecInputRemap22;
    case 23: return g_SpecInputRemap23;
    default: return 0u;
    }
}
#define g_InputRemap(LOCATION) renutInputRemap(LOCATION)
#else
#define g_InputRemap(LOCATION) 0u
#endif

// 2_10_10_10 signed normalized (N&B's normals, tangents and some texcoords):
// no host vertex format matches, so the host binds the 32 bits as R32_FLOAT
// and flags the input's location in g_PackedInputs.
float4 unpackInputDecoded(float4 value, uint location)
{
    if ((g_PackedInputs & (1u << location)) != 0)
    {
        uint v = asuint(value.x);
        int4 s = int4(int(v << 22) >> 22, int(v << 12) >> 22, int(v << 2) >> 22, int(v) >> 30);
        return max(float4(s) / float4(511.0, 511.0, 511.0, 1.0), -1.0);
    }
    // Integer formats read as numbers (UBYTE4, SHORT2/4): the host binds them
    // as UINT/SINT, which reaches a float input as raw bits, and the hardware
    // would have handed the shader the value as a float.
    if ((g_IntInputs & (1u << location)) != 0)
        return float4(asint(value));
    return value;
}

float4 unpackInput(float4 value, uint location)
{
    return renutRemap(unpackInputDecoded(value, location), g_InputRemap(location));
}

// Undo the engine bswap32 16-bit-pair swap (.yxwz) for any 16-bit-packed semantic flagged in the mask.
float4 swapFloats(uint swappedMask, float4 value, uint semanticIndex)
{
    return (swappedMask & (1u << semanticIndex)) != 0 ? value.yxwz : value;
}

// Recover X360 integer-cast-to-float TEXCOORDs from R16G16(B16A16)_UINT bindings (sign-extend low 16 bits).
float4 sintTexcoord(uint mask, float4 value, uint semanticIndex)
{
    if ((mask & (1u << semanticIndex)) != 0)
    {
        int4 si = (int4(asuint(value)) << 16) >> 16;
        return float4(si);
    }
    return value;
}
#else
float4 tfetchR11G11B10(uint4 value)
{
    if (g_SpecConstants() & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        return float4(
            (value.x & 0x00000400 ? -1.0 : 0.0) + ((value.x & 0x3FF) / 1024.0),
            (value.x & 0x00200000 ? -1.0 : 0.0) + (((value.x >> 11) & 0x3FF) / 1024.0),
            (value.x & 0x80000000 ? -1.0 : 0.0) + (((value.x >> 22) & 0x1FF) / 512.0),
            0.0);
    }
    else
    {
        return asfloat(value);
    }
}

float4 tfetchTexcoord(uint swappedTexcoords, float4 value, uint semanticIndex)
{
    return (swappedTexcoords & (1ull << semanticIndex)) != 0 ? value.yxwz : value;
}
#endif

float4 cube(float4 value, inout CubeMapData cubeMapData)
{
    uint index = cubeMapData.cubeMapIndex;
    cubeMapData.cubeMapDirections[index] = value.xyz;
    ++cubeMapData.cubeMapIndex;

    return float4(0.0, 0.0, 0.0, index);
}

float4 dst(float4 src0, float4 src1)
{
    float4 dest;
    dest.x = 1.0;
    dest.y = src0.y * src1.y;
    dest.z = src0.z;
    dest.w = src1.w;
    return dest;
}

// Returns the scalar it actually computes; HLSL broadcasts it across whatever
// destination swizzle the caller writes.
float max4(float4 src0)
{
    return max(max(src0.x, src0.y), max(src0.z, src0.w));
}

float2 getPixelCoord(uint resourceDescriptorIndex, float2 texCoord)
{
    return getTexture2DDimensions(g_Texture2DDescriptorHeap[resourceDescriptorIndex]) * texCoord;
}

float computeMipLevel(float2 pixelCoord)
{
    float2 dx = ddx(pixelCoord);
    float2 dy = ddy(pixelCoord);
    float deltaMaxSqr = max(dot(dx, dx), dot(dy, dy));
    return max(0.0, 0.5 * log2(deltaMaxSqr));
}

#endif

#endif
