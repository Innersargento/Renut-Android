#pragma once
// Format and state translation, Xenos to Vulkan.
#include "recipes.h"

namespace renut::gpu {


// A pipeline the driver would not build: Plume still hands back the object, with
// a null Vulkan handle, and binding that hangs some drivers. The callers tell
// failure from success by the pointer.
std::unique_ptr<RenderPipeline> CreateGraphicsPipeline(RenderDevice* device, const RenderGraphicsPipelineDesc& desc);

// A texture's subresource into a readback buffer, `row_texels` texels to a row.
// Plume's copyTextureRegion goes the other way only, so this is Vulkan on the
// command buffer itself; the texture is in COPY_SOURCE (transfer source).
void CopyTextureToBuffer(RenderCommandList* list, RenderBuffer* buffer, RenderTexture* texture, uint32_t width,
                         uint32_t height, uint32_t row_texels, uint32_t mip, uint32_t layer, const RenderBox* box);

RenderFormat ColorFormat(xenos::ColorRenderTargetFormat format);

bool IsBlendable(RenderFormat format);

// How a vertex element reaches the recompiled shader, whose inputs are all
// float4 like the hardware's vfetch results.
enum class InputDecode { kNone, kInteger, kPacked1010102, kPacked111110 };

// The host format for a guest vertex format, and what unpackInput() (or
// tfetchR11G11B10) must still do in the shader: integers read as numbers
// arrive as raw bits, and the packed formats have no host equivalent.
//
// Plume has no names for the converted-to-float formats a vertex shader that
// takes float inputs needs (UBYTE4, SHORT2, SHORT4 as numbers, and the packed
// 2_10_10_10), so they are asked for as the integer formats of the same width
// and the Vulkan layer (plume_renderer.cpp, LayerVertexFormat) creates the
// pipeline with the converting one: the 8 and 16-bit UINT/SINT formats become
// USCALED/SSCALED, and the two 8-bit two-channel formats nothing else uses as an
// input stand for the 2_10_10_10 ones.
constexpr RenderFormat kVertexUnorm1010102 = RenderFormat::R8G8_UINT;
constexpr RenderFormat kVertexUscaled1010102 = RenderFormat::R8G8_SINT;

RenderFormat VertexFormat(uint32_t format, bool is_signed, bool normalized, InputDecode& decode);

bool IsSixteenBitFormat(uint32_t format);

// Xenos k_11_11_10 (vertex format 17) keeps X in the low 10 bits and Y and Z
// in 11 bits each; k_10_11_11 (16) is the other way round (X and Y 11 bits, Z
// 10). tfetchR11G11B10 in the recompiled shaders decodes only the second
// layout, so a format 17 vector - the ground decals' tangent and binormal -
// came out a different, non-unit vector for every small change of its bits,
// and the tread ribbon's relief and light jumped from quad to quad. Move the
// same vector into the layout the shader reads.
uint32_t RepackFormat17(uint32_t word);

RenderBlend Blend(xenos::BlendFactor factor);

RenderBlendOperation BlendOp(xenos::BlendOp op);

RenderComparisonFunction Compare(xenos::CompareFunction f);

}  // namespace renut::gpu
