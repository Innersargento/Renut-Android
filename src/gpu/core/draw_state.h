#pragma once
// The state a draw runs with: what the game's Direct3D device holds (render states, viewport,
// scissor, shader constants, fetch constants), by name and type - only the words the backend reads.
//
// The front end (d3d/frontend/) sends changes only: a record names a run of words by its offset
// here (RENUT_STATE_WORD) and carries their values, as the device holds them - the device keeps its
// state words in the Xenos format (state_bits.h has the layouts). The device's blocks have more words
// than are kept here (the surfaces are named by kOpSurfaces, the resolve by kOpResolve, the shader
// program by kOpShader): the front end's tables (flush.cpp) say where each word that is kept goes, and
// the others are not sent. The backend applies the record to its DrawState (Lists::NativeWrite) and a
// draw reads it; there is no register file and no register index anywhere between the two.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "gpu/core/state_bits.h"

namespace renut::gpu {

struct DrawState {
  // Screen scissor (the targets block's last two words; Direct3D 9 leaves it at 0, 0 - 8192, 8192).
  bits::PA_SC_SCREEN_SCISSOR_TL screen_scissor_tl;
  bits::PA_SC_SCREEN_SCISSOR_BR screen_scissor_br;

  // Window offset and scissor, 3 words in this order (written by SetSurfaceClip).
  bits::PA_SC_WINDOW_OFFSET window_offset;
  bits::PA_SC_WINDOW_SCISSOR_TL window_scissor_tl;
  bits::PA_SC_WINDOW_SCISSOR_BR window_scissor_br;

  // Output merger and viewport.
  uint32_t base_vertex;
  bits::RB_COLOR_MASK color_mask;
  float blend_color[4];  // red, green, blue, alpha
  bits::RB_STENCILREFMASK stencil_ref_mask_bf;
  bits::RB_STENCILREFMASK stencil_ref_mask;
  float alpha_ref;
  float vport_x_scale, vport_x_offset, vport_y_scale, vport_y_offset, vport_z_scale, vport_z_offset;

  // Depth, blend, clip and raster mode.
  bits::RB_DEPTHCONTROL depth_control;
  bits::RB_BLENDCONTROL blend_control0;
  bits::RB_COLORCONTROL color_control;
  bits::PA_CL_CLIP_CNTL clip_cntl;
  bits::PA_SU_SC_MODE_CNTL raster;
  bits::PA_CL_VTE_CNTL vte_cntl;
  bits::RB_BLENDCONTROL blend_control123[3];

  // Points and tessellation.
  bits::PA_SU_POINT_SIZE point_size;
  bits::PA_SU_POINT_MINMAX point_minmax;
  bits::VGT_OUTPUT_PATH_CNTL output_path_cntl;
  bits::VGT_HOS_CNTL hos_cntl;
  float hos_max_tess_level;
  float hos_min_tess_level;
  bits::PA_SC_VIZ_QUERY viz_query;

  // Vertex control and the constant ranges.
  bits::PA_SU_VTX_CNTL vtx_cntl;
  bits::SQ_VS_CONST sq_vs_const;
  bits::SQ_VS_CONST sq_ps_const;

  // Polygon offset, then the six user clip planes.
  float poly_offset_front_scale, poly_offset_front_offset, poly_offset_back_scale, poly_offset_back_offset;
  float clip_planes[6][4];

  // The draw itself (kOpDraw): the initiator and the index buffer's DMA.
  bits::VGT_DRAW_INITIATOR draw_initiator;
  uint32_t dma_base;
  bits::VGT_DMA_SIZE dma_size;

  // ---- Constants: the shaders' float registers (vertex 0, pixel 1024), the fetch constants
  // (32 textures, or the vertex fetches, six words each) and the booleans and loops. A write
  // is compared with what is here and marks the block that changed (Draw::MarkConstantsWritten). ----
  uint32_t float_constants[2048];
  uint32_t fetch[192];
  uint32_t bools[8];
  uint32_t loops[32];

  DrawState() { std::memset(static_cast<void*>(this), 0, sizeof(*this)); }

  static constexpr uint32_t kPixelConstants = 1024;

  const bits::RB_BLENDCONTROL& BlendControl(uint32_t target) const {
    return target ? blend_control123[target - 1] : blend_control0;
  }
  float VertexFloat(uint32_t i) const { return AsFloat(float_constants[i]); }
  float PixelFloat(uint32_t i) const { return AsFloat(float_constants[kPixelConstants + i]); }
  const uint32_t* VertexConstants() const { return float_constants; }
  const uint32_t* PixelConstants() const { return float_constants + kPixelConstants; }

  xenos::xe_gpu_vertex_fetch_t GetVertexFetch(uint32_t index) const {
    xenos::xe_gpu_vertex_fetch_t result;
    std::memcpy(&result, &fetch[(sizeof(result) / sizeof(uint32_t)) * index], sizeof(result));
    return result;
  }
  xenos::xe_gpu_texture_fetch_t GetTextureFetch(uint32_t index) const {
    xenos::xe_gpu_texture_fetch_t result;
    std::memcpy(&result, &fetch[(sizeof(result) / sizeof(uint32_t)) * index], sizeof(result));
    return result;
  }

  static float AsFloat(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }
};

// Where a member sits, in dwords from the start of the DrawState: what a state record names.
#define RENUT_STATE_WORD(member) (uint32_t(offsetof(::renut::gpu::DrawState, member) / sizeof(uint32_t)))

// The constants' words are the last in the DrawState and in one run: float constants (vertex,
// then pixel), the fetch constants, the booleans and the loops.
constexpr uint32_t kStateConstantsFirst = RENUT_STATE_WORD(float_constants);
constexpr uint32_t kStateLoopsFirst = RENUT_STATE_WORD(loops);
constexpr uint32_t kStateWords = uint32_t(sizeof(DrawState) / sizeof(uint32_t));

static_assert(sizeof(DrawState) % sizeof(uint32_t) == 0);
static_assert(sizeof(float) == sizeof(uint32_t));
// The front end sends these as runs (flush.cpp's tables).
static_assert(RENUT_STATE_WORD(window_scissor_tl) == RENUT_STATE_WORD(window_offset) + 1);
static_assert(RENUT_STATE_WORD(window_scissor_br) == RENUT_STATE_WORD(window_offset) + 2);
static_assert(RENUT_STATE_WORD(screen_scissor_br) == RENUT_STATE_WORD(screen_scissor_tl) + 1);
static_assert(RENUT_STATE_WORD(vport_z_offset) == RENUT_STATE_WORD(vport_x_scale) + 5);
static_assert(RENUT_STATE_WORD(blend_color) + 4 == RENUT_STATE_WORD(stencil_ref_mask_bf));
static_assert(RENUT_STATE_WORD(clip_planes) == RENUT_STATE_WORD(poly_offset_back_offset) + 1);
static_assert(RENUT_STATE_WORD(loops) == RENUT_STATE_WORD(bools) + 8);
static_assert(RENUT_STATE_WORD(loops) + 32 == kStateWords);
static_assert(RENUT_STATE_WORD(fetch) == RENUT_STATE_WORD(float_constants) + 2048);
static_assert(RENUT_STATE_WORD(bools) == RENUT_STATE_WORD(fetch) + 192);
static_assert(DrawState::kPixelConstants * 2 == sizeof(DrawState::float_constants) / sizeof(uint32_t));

}  // namespace renut::gpu
