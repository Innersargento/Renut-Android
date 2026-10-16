/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    reNut - taken into the app (renut::gpu::draw_util), without the Shader-dependent
 *              helpers and the host viewport code
 */

#include <algorithm>
#include <cmath>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/registers.h>
#include "draw_util.h"
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/ui/graphics_util.h>

REXCVAR_DEFINE_BOOL(half_pixel_offset, true, "GPU", "Enable half pixel offset");

REXCVAR_DEFINE_BOOL(resolve_resolution_scale_fill_half_pixel_offset, true, "GPU",
                    "Fill half pixel offset during resolution scale resolve");

// Very prominent in 545407F2.
// DEFINE_bool(
//     resolve_resolution_scale_fill_half_pixel_offset, true,
//     "When using resolution scaling, apply the hack that stretches the first "
//     "surely covered host pixel in the left and top sides of render target "
//     "resolve areas to eliminate the gap caused by the half-pixel offset (this "
//     "is necessary for certain games to display the scene graphics).",
//     "GPU");

namespace renut::gpu::draw_util {
using namespace rex::graphics;
namespace memory = rex::memory;
namespace ui = rex::ui;


reg::RB_DEPTHCONTROL GetNormalizedDepthControl(const RegisterFile& regs) {
  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode != xenos::EdramMode::kColorDepth && edram_mode != xenos::EdramMode::kDepthOnly) {
    // Both depth and stencil disabled (EDRAM depth and stencil ignored).
    reg::RB_DEPTHCONTROL disabled;
    disabled.value = 0;
    return disabled;
  }
  reg::RB_DEPTHCONTROL depthcontrol = regs.Get<reg::RB_DEPTHCONTROL>();
  // For more reliable skipping of depth render target management for draws not
  // requiring depth.
  if (depthcontrol.z_enable && !depthcontrol.z_write_enable &&
      depthcontrol.zfunc == xenos::CompareFunction::kAlways) {
    depthcontrol.z_enable = 0;
  }
  // Stencil is more complex and is expected to be usually enabled explicitly
  // when needed.
  return depthcontrol;
}

// https://docs.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_standard_multisample_quality_levels
const int8_t kD3D10StandardSamplePositions2x[2][2] = {{4, 4}, {-4, -4}};
const int8_t kD3D10StandardSamplePositions4x[4][2] = {{-2, -6}, {6, -2}, {-6, 2}, {2, 6}};

void GetPreferredFacePolygonOffset(const RegisterFile& regs, bool primitive_polygonal,
                                   float& scale_out, float& offset_out) {
  float scale = 0.0f, offset = 0.0f;
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (primitive_polygonal) {
    // Prefer the front polygon offset because in general, front faces are the
    // ones that are rendered (except for shadow volumes).
    if (pa_su_sc_mode_cntl.poly_offset_front_enable && !pa_su_sc_mode_cntl.cull_front) {
      scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
      offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
    }
    if (pa_su_sc_mode_cntl.poly_offset_back_enable && !pa_su_sc_mode_cntl.cull_back && !scale &&
        !offset) {
      scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
      offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
    }
  } else {
    // Non-triangle primitives use the front offset, but it's toggled via
    // poly_offset_para_enable.
    if (pa_su_sc_mode_cntl.poly_offset_para_enable) {
      scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
      offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
    }
  }
  scale_out = scale;
  offset_out = offset;
}

void GetScissor(const RegisterFile& regs, Scissor& scissor_out, bool clamp_to_surface_pitch) {
  auto pa_sc_window_scissor_tl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
  int32_t tl_x = int32_t(pa_sc_window_scissor_tl.tl_x);
  int32_t tl_y = int32_t(pa_sc_window_scissor_tl.tl_y);
  auto pa_sc_window_scissor_br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
  int32_t br_x = int32_t(pa_sc_window_scissor_br.br_x);
  int32_t br_y = int32_t(pa_sc_window_scissor_br.br_y);
  if (!pa_sc_window_scissor_tl.window_offset_disable) {
    auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    tl_x += pa_sc_window_offset.window_x_offset;
    tl_y += pa_sc_window_offset.window_y_offset;
    br_x += pa_sc_window_offset.window_x_offset;
    br_y += pa_sc_window_offset.window_y_offset;
  }
  // Screen scissor is not used by Direct3D 9 (always 0, 0 to 8192, 8192), but
  // still handled here for completeness.
  auto pa_sc_screen_scissor_tl = regs.Get<reg::PA_SC_SCREEN_SCISSOR_TL>();
  tl_x = std::max(tl_x, int32_t(pa_sc_screen_scissor_tl.tl_x));
  tl_y = std::max(tl_y, int32_t(pa_sc_screen_scissor_tl.tl_y));
  auto pa_sc_screen_scissor_br = regs.Get<reg::PA_SC_SCREEN_SCISSOR_BR>();
  br_x = std::min(br_x, int32_t(pa_sc_screen_scissor_br.br_x));
  br_y = std::min(br_y, int32_t(pa_sc_screen_scissor_br.br_y));
  if (clamp_to_surface_pitch) {
    // Clamp the horizontal scissor to surface_pitch for safety, in case that's
    // not done by the guest for some reason (it's not when doing draws without
    // clipping in Direct3D 9, for instance), to prevent overflow - this is
    // important for host implementations, both based on target-indepedent
    // rasterization without render target width at all (pixel shader
    // interlock-based custom RB implementations) and using conventional render
    // targets, but padded to EDRAM tiles.
    uint32_t surface_pitch = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch;
    tl_x = std::min(tl_x, int32_t(surface_pitch));
    br_x = std::min(br_x, int32_t(surface_pitch));
  }
  // Ensure the rectangle is non-negative, by collapsing it into a 0-sized one
  // (not by reordering the bounds preserving the width / height, which would
  // reveal samples not meant to be covered, unless TL > BR does that on a real
  // console, but no evidence of such has ever been seen), and also drop
  // negative offsets.
  tl_x = std::max(tl_x, int32_t(0));
  tl_y = std::max(tl_y, int32_t(0));
  br_x = std::max(br_x, tl_x);
  br_y = std::max(br_y, tl_y);
  scissor_out.offset[0] = uint32_t(tl_x);
  scissor_out.offset[1] = uint32_t(tl_y);
  scissor_out.extent[0] = uint32_t(br_x - tl_x);
  scissor_out.extent[1] = uint32_t(br_y - tl_y);
}

xenos::CopySampleSelect SanitizeCopySampleSelect(xenos::CopySampleSelect copy_sample_select,
                                                 xenos::MsaaSamples msaa_samples, bool is_depth) {
  // Depth can't be averaged.
  if (msaa_samples >= xenos::MsaaSamples::k4X) {
    if (copy_sample_select > xenos::CopySampleSelect::k0123) {
      copy_sample_select = xenos::CopySampleSelect::k0123;
    }
    if (is_depth) {
      switch (copy_sample_select) {
        case xenos::CopySampleSelect::k01:
        case xenos::CopySampleSelect::k0123:
          copy_sample_select = xenos::CopySampleSelect::k0;
          break;
        case xenos::CopySampleSelect::k23:
          copy_sample_select = xenos::CopySampleSelect::k2;
          break;
        default:
          break;
      }
    }
  } else if (msaa_samples >= xenos::MsaaSamples::k2X) {
    switch (copy_sample_select) {
      case xenos::CopySampleSelect::k2:
        copy_sample_select = xenos::CopySampleSelect::k0;
        break;
      case xenos::CopySampleSelect::k3:
        copy_sample_select = xenos::CopySampleSelect::k1;
        break;
      default:
        if (copy_sample_select > xenos::CopySampleSelect::k01) {
          copy_sample_select = xenos::CopySampleSelect::k01;
        }
    }
    if (is_depth && copy_sample_select == xenos::CopySampleSelect::k01) {
      copy_sample_select = xenos::CopySampleSelect::k0;
    }
  } else {
    copy_sample_select = xenos::CopySampleSelect::k0;
  }
  return copy_sample_select;
}

void GetResolveEdramTileSpan(ResolveEdramInfo edram_info, ResolveCoordinateInfo coordinate_info,
                             uint32_t height_div_8, uint32_t& base_out,
                             uint32_t& row_length_used_out, uint32_t& rows_out) {
  // Due to 64bpp, and also not to make an assumption that the offsets are
  // limited to (80 - 8, 8 - 8) with 2x MSAA, and (40 - 8, 8 - 8) with 4x MSAA,
  // still taking the offset into account.
  uint32_t x_scale_log2 =
      3 + uint32_t(edram_info.msaa_samples >= xenos::MsaaSamples::k4X) + edram_info.format_is_64bpp;
  uint32_t x0 =
      (coordinate_info.edram_offset_x_div_8 << x_scale_log2) / xenos::kEdramTileWidthSamples;
  uint32_t x1 =
      (((coordinate_info.edram_offset_x_div_8 + coordinate_info.width_div_8) << x_scale_log2) +
       (xenos::kEdramTileWidthSamples - 1)) /
      xenos::kEdramTileWidthSamples;
  uint32_t y_scale_log2 = 3 + uint32_t(edram_info.msaa_samples >= xenos::MsaaSamples::k2X);
  uint32_t y0 =
      (coordinate_info.edram_offset_y_div_8 << y_scale_log2) / xenos::kEdramTileHeightSamples;
  uint32_t y1 = (((coordinate_info.edram_offset_y_div_8 + height_div_8) << y_scale_log2) +
                 (xenos::kEdramTileHeightSamples - 1)) /
                xenos::kEdramTileHeightSamples;
  base_out = edram_info.base_tiles + y0 * edram_info.pitch_tiles + x0;
  row_length_used_out = x1 - x0;
  rows_out = y1 - y0;
}

const ResolveCopyShaderInfo resolve_copy_shader_info[size_t(ResolveCopyShaderIndex::kCount)] = {
    {"Resolve Copy Fast 32bpp 1x/2xMSAA", false, 4, 4, 6, 3},
    {"Resolve Copy Fast 32bpp 4xMSAA", false, 4, 4, 6, 3},
    {"Resolve Copy Fast 64bpp 1x/2xMSAA", false, 4, 4, 5, 3},
    {"Resolve Copy Fast 64bpp 4xMSAA", false, 3, 4, 5, 3},
    {"Resolve Copy Full 8bpp", true, 2, 3, 6, 3},
    {"Resolve Copy Full 16bpp", true, 2, 3, 5, 3},
    {"Resolve Copy Full 32bpp", true, 2, 4, 5, 3},
    {"Resolve Copy Full 64bpp", true, 2, 4, 5, 3},
    {"Resolve Copy Full 128bpp", true, 2, 4, 4, 3},
};

bool GetResolveInfo(const RegisterFile& regs, const memory::Memory& memory,
                    uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y,
                    bool fixed_rg16_truncated_to_minus_1_to_1,
                    bool fixed_rgba16_truncated_to_minus_1_to_1, ResolveInfo& info_out) {
  // Don't pass uninitialized values to shaders, not to leak data to frame
  // captures. Also initialize an invalid resolve to empty.
  info_out.coordinate_info.packed = 0;
  info_out.height_div_8 = 0;

  auto rb_copy_control = regs.Get<reg::RB_COPY_CONTROL>();
  info_out.rb_copy_control = rb_copy_control;

  if (rb_copy_control.copy_command != xenos::CopyCommand::kRaw &&
      rb_copy_control.copy_command != xenos::CopyCommand::kConvert) {
    REXGPU_ERROR(
        "Unsupported resolve copy command {}. Report the game to Xenia "
        "developers",
        uint32_t(rb_copy_control.copy_command));
    assert_always();
    return false;
  }

  // Get the extent of pixels covered by the resolve rectangle, according to the
  // top-left rasterization rule.
  // D3D9 HACK: Vertices to use are always in vf0, and are written by the CPU.
  xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(0);
  if (fetch.type != xenos::FetchConstantType::kVertex || fetch.size != 3 * 2) {
    REXGPU_ERROR("Unsupported resolve vertex buffer format");
    assert_always();
    return false;
  }
  const float* vertices_guest =
      reinterpret_cast<const float*>(memory.TranslatePhysical(fetch.address * sizeof(uint32_t)));
  // Most vertices have a negative half-pixel offset applied, which we reverse.
  float half_pixel_offset =
      regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f : 0.0f;
  int32_t vertices_fixed[6];
  for (size_t i = 0; i < rex::countof(vertices_fixed); ++i) {
    vertices_fixed[i] = ui::FloatToD3D11Fixed16p8(xenos::GpuSwap(vertices_guest[i], fetch.endian) +
                                                  half_pixel_offset);
  }
  // Inclusive.
  int32_t x0 = std::min(std::min(vertices_fixed[0], vertices_fixed[2]), vertices_fixed[4]);
  int32_t y0 = std::min(std::min(vertices_fixed[1], vertices_fixed[3]), vertices_fixed[5]);
  // Exclusive.
  int32_t x1 = std::max(std::max(vertices_fixed[0], vertices_fixed[2]), vertices_fixed[4]);
  int32_t y1 = std::max(std::max(vertices_fixed[1], vertices_fixed[3]), vertices_fixed[5]);
  // Top-left - include .5 (0.128 treated as 0 covered, 0.129 as 0 not covered).
  x0 = (x0 + 127) >> 8;
  y0 = (y0 + 127) >> 8;
  // Bottom-right - exclude .5.
  x1 = (x1 + 127) >> 8;
  y1 = (y1 + 127) >> 8;

  auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();

  // Apply the window offset to the vertices.
  if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
    x0 += pa_sc_window_offset.window_x_offset;
    y0 += pa_sc_window_offset.window_y_offset;
    x1 += pa_sc_window_offset.window_x_offset;
    y1 += pa_sc_window_offset.window_y_offset;
  }

  // Apply the scissor and prevent negative origin (behind the EDRAM base).
  Scissor scissor;
  // False because clamping to the surface pitch will be done later (it will be
  // aligned to the resolve alignment here, for resolving from render targets
  // with a pitch that is not a multiple of 8).
  GetScissor(regs, scissor, false);
  int32_t scissor_right = int32_t(scissor.offset[0] + scissor.extent[0]);
  int32_t scissor_bottom = int32_t(scissor.offset[1] + scissor.extent[1]);
  x0 = std::clamp(x0, int32_t(scissor.offset[0]), scissor_right);
  y0 = std::clamp(y0, int32_t(scissor.offset[1]), scissor_bottom);
  x1 = std::clamp(x1, int32_t(scissor.offset[0]), scissor_right);
  y1 = std::clamp(y1, int32_t(scissor.offset[1]), scissor_bottom);

  assert_true(x0 <= x1 && y0 <= y1);

  // Direct3D 9's D3DDevice_Resolve internally rounds the right/bottom of the
  // rectangle internally to 8. While all the alignment should have already been
  // done by Direct3D 9, just for safety of host implementation of resolve,
  // force-align the rectangle by expanding (D3D9 expands to the right/bottom
  // for some reason, haven't found how left/top is rounded, but logically it
  // would make sense to expand to the left/top too).
  x0 &= ~int32_t(xenos::kResolveAlignmentPixels - 1);
  y0 &= ~int32_t(xenos::kResolveAlignmentPixels - 1);
  x1 = rex::align(x1, int32_t(xenos::kResolveAlignmentPixels));
  y1 = rex::align(y1, int32_t(xenos::kResolveAlignmentPixels));

  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  if (rb_surface_info.msaa_samples > xenos::MsaaSamples::k4X) {
    // Safety check because a lot of code assumes up to 4x.
    assert_always();
    REXGPU_ERROR(
        "{}x MSAA requested by the guest in a resolve, Xenos only supports up "
        "to 4x",
        uint32_t(1) << uint32_t(rb_surface_info.msaa_samples));
    return false;
  }

  // Clamp to the EDRAM surface pitch (maximum possible surface pitch is also
  // assumed to be the largest resolvable size).
  int32_t surface_pitch_aligned =
      int32_t(rb_surface_info.surface_pitch & ~uint32_t(xenos::kResolveAlignmentPixels - 1));
  if (x1 > surface_pitch_aligned) {
    REXGPU_ERROR("Resolve region {} <= x < {} is outside the surface pitch {}", x0, x1,
                 surface_pitch_aligned);
    x0 = std::min(x0, surface_pitch_aligned);
    x1 = std::min(x1, surface_pitch_aligned);
  }
  assert_true(x1 - x0 <= int32_t(xenos::kMaxResolveSize));

  // Clamp the height to a sane value (to make sure it can fit in the packed
  // shader constant).
  if (y1 - y0 > int32_t(xenos::kMaxResolveSize)) {
    REXGPU_ERROR("Resolve region {} <= y < {} is taller than {}", y0, y1, xenos::kMaxResolveSize);
    y1 = y0 + int32_t(xenos::kMaxResolveSize);
  }

  assert_true(x0 < x1 && y0 < y1);
  if (x0 >= x1 || y0 >= y1) {
    REXGPU_ERROR("Resolve region is empty");
    return false;
  }

  info_out.coordinate_info.width_div_8 = uint32_t(x1 - x0) >> xenos::kResolveAlignmentPixelsLog2;
  info_out.height_div_8 = uint32_t(y1 - y0) >> xenos::kResolveAlignmentPixelsLog2;
  // 3 bits for each.
  assert_true(draw_resolution_scale_x <= 7);
  assert_true(draw_resolution_scale_y <= 7);
  info_out.coordinate_info.draw_resolution_scale_x = draw_resolution_scale_x;
  info_out.coordinate_info.draw_resolution_scale_y = draw_resolution_scale_y;

  // Handle the destination.
  bool is_depth = rb_copy_control.copy_src_select >= xenos::kMaxColorRenderTargets;
  // Get the sample selection to safely pass to the shader.
  xenos::CopySampleSelect sample_select = SanitizeCopySampleSelect(
      rb_copy_control.copy_sample_select, rb_surface_info.msaa_samples, is_depth);
  if (rb_copy_control.copy_sample_select != sample_select) {
    REXGPU_WARN(
        "Incorrect resolve sample selected for {}-sample {}: {}, treating like "
        "{}",
        1 << uint32_t(rb_surface_info.msaa_samples), is_depth ? "depth" : "color",
        static_cast<uint32_t>(rb_copy_control.copy_sample_select),
        static_cast<uint32_t>(sample_select));
  }
  info_out.copy_dest_coordinate_info.copy_sample_select = sample_select;
  // Get the format to pass to the shader in a unified way - for depth (for
  // which Direct3D 9 specifies the k_8_8_8_8 uint destination format), make
  // sure the shader won't try to do conversion - pass proper k_24_8 or
  // k_24_8_FLOAT.
  auto rb_copy_dest_info = regs.Get<reg::RB_COPY_DEST_INFO>();
  xenos::TextureFormat dest_format;
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  if (is_depth) {
    dest_format = DepthRenderTargetToTextureFormat(rb_depth_info.depth_format);
  } else {
    dest_format = xenos::TextureFormat(rb_copy_dest_info.copy_dest_format);
    // For development feedback - not much known about these formats currently.
    xenos::TextureFormat dest_closest_format;
    switch (dest_format) {
      case xenos::TextureFormat::k_8_A:
      case xenos::TextureFormat::k_8_B:
        dest_closest_format = xenos::TextureFormat::k_8;
        break;
      case xenos::TextureFormat::k_8_8_8_8_A:
        dest_closest_format = xenos::TextureFormat::k_8_8_8_8;
        break;
      default:
        dest_closest_format = dest_format;
    }
    if (dest_format != dest_closest_format) {
      REXGPU_WARN(
          "Resolving to format {}, which is untested - treating like {}. "
          "Report the game to Xenia developers!",
          FormatInfo::Get(dest_format)->name, FormatInfo::Get(dest_closest_format)->name);
    }
  }

  // Calculate the destination memory extent.
  uint32_t rb_copy_dest_base = regs[XE_GPU_REG_RB_COPY_DEST_BASE];
  uint32_t copy_dest_base_adjusted = rb_copy_dest_base;
  uint32_t copy_dest_extent_start, copy_dest_extent_end;
  auto rb_copy_dest_pitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
  uint32_t copy_dest_pitch_aligned_div_32 =
      (rb_copy_dest_pitch.copy_dest_pitch + (xenos::kTextureTileWidthHeight - 1)) >>
      xenos::kTextureTileWidthHeightLog2;
  info_out.copy_dest_coordinate_info.pitch_aligned_div_32 = copy_dest_pitch_aligned_div_32;
  info_out.copy_dest_coordinate_info.height_aligned_div_32 =
      (rb_copy_dest_pitch.copy_dest_height + (xenos::kTextureTileWidthHeight - 1)) >>
      xenos::kTextureTileWidthHeightLog2;
  const FormatInfo& dest_format_info = *FormatInfo::Get(dest_format);
  if (is_depth || dest_format_info.type == FormatType::kResolvable) {
    uint32_t bpp_log2 = rex::log2_floor(dest_format_info.bits_per_pixel >> 3);
    uint32_t dest_base_relative_x_mask = (UINT32_C(1) << xenos::GetTextureTiledXBaseGranularityLog2(
                                              bool(rb_copy_dest_info.copy_dest_array), bpp_log2)) -
                                         1;
    uint32_t dest_base_relative_y_mask = (UINT32_C(1) << xenos::GetTextureTiledYBaseGranularityLog2(
                                              bool(rb_copy_dest_info.copy_dest_array), bpp_log2)) -
                                         1;
    info_out.copy_dest_coordinate_info.offset_x_div_8 =
        (uint32_t(x0) & dest_base_relative_x_mask) >> xenos::kResolveAlignmentPixelsLog2;
    info_out.copy_dest_coordinate_info.offset_y_div_8 =
        (uint32_t(y0) & dest_base_relative_y_mask) >> xenos::kResolveAlignmentPixelsLog2;
    uint32_t dest_base_x = uint32_t(x0) & ~dest_base_relative_x_mask;
    uint32_t dest_base_y = uint32_t(y0) & ~dest_base_relative_y_mask;
    if (rb_copy_dest_info.copy_dest_array) {
      // The base pointer is already adjusted to the Z / 8 (copy_dest_slice is
      // 3-bit).
      copy_dest_base_adjusted += texture_util::GetTiledOffset3D(
          int32_t(dest_base_x), int32_t(dest_base_y), 0, rb_copy_dest_pitch.copy_dest_pitch,
          rb_copy_dest_pitch.copy_dest_height, bpp_log2);
      copy_dest_extent_start =
          rb_copy_dest_base + texture_util::GetTiledAddressLowerBound3D(
                                  uint32_t(x0), uint32_t(y0), rb_copy_dest_info.copy_dest_slice,
                                  rb_copy_dest_pitch.copy_dest_pitch,
                                  rb_copy_dest_pitch.copy_dest_height, bpp_log2);
      copy_dest_extent_end =
          rb_copy_dest_base + texture_util::GetTiledAddressUpperBound3D(
                                  uint32_t(x1), uint32_t(y1), rb_copy_dest_info.copy_dest_slice + 1,
                                  rb_copy_dest_pitch.copy_dest_pitch,
                                  rb_copy_dest_pitch.copy_dest_height, bpp_log2);
    } else {
      copy_dest_base_adjusted += texture_util::GetTiledOffset2D(
          int32_t(dest_base_x), int32_t(dest_base_y), rb_copy_dest_pitch.copy_dest_pitch, bpp_log2);
      copy_dest_extent_start =
          rb_copy_dest_base +
          texture_util::GetTiledAddressLowerBound2D(uint32_t(x0), uint32_t(y0),
                                                    rb_copy_dest_pitch.copy_dest_pitch, bpp_log2);
      copy_dest_extent_end = rb_copy_dest_base + texture_util::GetTiledAddressUpperBound2D(
                                                     uint32_t(x1), uint32_t(y1),
                                                     rb_copy_dest_pitch.copy_dest_pitch, bpp_log2);
    }
  } else {
    REXGPU_ERROR("Tried to resolve to format {}, which is not a ColorFormat",
                 dest_format_info.name);
    copy_dest_extent_start = copy_dest_base_adjusted;
    copy_dest_extent_end = copy_dest_base_adjusted;
  }
  assert_true(copy_dest_extent_start >= copy_dest_base_adjusted);
  assert_true(copy_dest_extent_end >= copy_dest_base_adjusted);
  assert_true(copy_dest_extent_end >= copy_dest_extent_start);
  info_out.copy_dest_base = copy_dest_base_adjusted;
  info_out.copy_dest_extent_start = copy_dest_extent_start;
  info_out.copy_dest_extent_length = copy_dest_extent_end - copy_dest_extent_start;

  // Offset relative to the beginning of the tile to put it in fewer bits.
  uint32_t sample_count_log2_x = uint32_t(rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X);
  uint32_t sample_count_log2_y = uint32_t(rb_surface_info.msaa_samples >= xenos::MsaaSamples::k2X);
  uint32_t x0_samples = uint32_t(x0) << sample_count_log2_x;
  uint32_t y0_samples = uint32_t(y0) << sample_count_log2_y;
  uint32_t base_offset_x_tiles = x0_samples / xenos::kEdramTileWidthSamples;
  uint32_t base_offset_y_tiles = y0_samples / xenos::kEdramTileHeightSamples;
  info_out.coordinate_info.edram_offset_x_div_8 =
      (x0_samples % xenos::kEdramTileWidthSamples) >> (sample_count_log2_x + 3);
  info_out.coordinate_info.edram_offset_y_div_8 =
      (y0_samples % xenos::kEdramTileHeightSamples) >> (sample_count_log2_y + 3);
  uint32_t surface_pitch_tiles = xenos::GetSurfacePitchTiles(rb_surface_info.surface_pitch,
                                                             rb_surface_info.msaa_samples, false);
  uint32_t edram_base_offset_tiles =
      base_offset_y_tiles * surface_pitch_tiles + base_offset_x_tiles;

  // Write the color/depth EDRAM info.
  bool fill_half_pixel_offset =
      (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) &&
      REXCVAR_GET(resolve_resolution_scale_fill_half_pixel_offset) &&
      REXCVAR_GET(half_pixel_offset) &&
      regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero;
  int32_t exp_bias = is_depth ? 0 : rb_copy_dest_info.copy_dest_exp_bias;
  ResolveEdramInfo depth_edram_info;
  depth_edram_info.packed = 0;
  if (is_depth || rb_copy_control.depth_clear_enable) {
    depth_edram_info.pitch_tiles = surface_pitch_tiles;
    depth_edram_info.msaa_samples = rb_surface_info.msaa_samples;
    depth_edram_info.is_depth = 1;
    // If wrapping happens, it's fine, it doesn't matter how many times and
    // where modulo xenos::kEdramTileCount is applied in this context.
    depth_edram_info.base_tiles = rb_depth_info.depth_base + edram_base_offset_tiles;
    depth_edram_info.format = uint32_t(rb_depth_info.depth_format);
    depth_edram_info.format_is_64bpp = 0;
    depth_edram_info.fill_half_pixel_offset = uint32_t(fill_half_pixel_offset);
    info_out.depth_original_base = rb_depth_info.depth_base;
  } else {
    info_out.depth_original_base = 0;
  }
  info_out.depth_edram_info = depth_edram_info;
  ResolveEdramInfo color_edram_info;
  color_edram_info.packed = 0;
  if (!is_depth) {
    // Color.
    auto color_info = regs.Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[rb_copy_control.copy_src_select]);
    uint32_t is_64bpp = uint32_t(xenos::IsColorRenderTargetFormat64bpp(color_info.color_format));
    color_edram_info.pitch_tiles = surface_pitch_tiles << is_64bpp;
    color_edram_info.msaa_samples = rb_surface_info.msaa_samples;
    color_edram_info.is_depth = 0;
    // If wrapping happens, it's fine, it doesn't matter how many times and
    // where modulo xenos::kEdramTileCount is applied in this context.
    color_edram_info.base_tiles = color_info.color_base + (edram_base_offset_tiles << is_64bpp);
    color_edram_info.format = uint32_t(color_info.color_format);
    color_edram_info.format_is_64bpp = is_64bpp;
    color_edram_info.fill_half_pixel_offset = uint32_t(fill_half_pixel_offset);
    if ((fixed_rg16_truncated_to_minus_1_to_1 &&
         color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16) ||
        (fixed_rgba16_truncated_to_minus_1_to_1 &&
         color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16)) {
      // The texture expects 0x8001 = -32, 0x7FFF = 32, but the hack making
      // 0x8001 = -1, 0x7FFF = 1 is used - revert (this won't be correct if the
      // requested exponent bias is 27 or above, but it's a hack anyway, no need
      // to create a new copy info structure with one more bit just for this).
      exp_bias = std::min(exp_bias + int32_t(5), int32_t(31));
    }
    info_out.color_original_base = color_info.color_base;
  } else {
    info_out.color_original_base = 0;
  }
  info_out.color_edram_info = color_edram_info;

  // Patch and write RB_COPY_DEST_INFO.
  info_out.copy_dest_info = rb_copy_dest_info;
  // Override with the depth format to make sure the shader doesn't have any
  // reason to try to do k_8_8_8_8 packing.
  info_out.copy_dest_info.copy_dest_format = xenos::ColorFormat(dest_format);
  // Handle k_16_16 and k_16_16_16_16 range.
  info_out.copy_dest_info.copy_dest_exp_bias = exp_bias;
  if (is_depth) {
    // Single component, nothing to swap.
    info_out.copy_dest_info.copy_dest_swap = false;
  }

  info_out.rb_depth_clear = regs[XE_GPU_REG_RB_DEPTH_CLEAR];
  info_out.rb_color_clear = regs[XE_GPU_REG_RB_COLOR_CLEAR];
  info_out.rb_color_clear_lo = regs[XE_GPU_REG_RB_COLOR_CLEAR_LO];

  REXGPU_TRACE(
      "Resolve: {},{} <= x,y < {},{}, {} -> {} at 0x{:08X} (potentially "
      "modified memory range 0x{:08X} to 0x{:08X})",
      x0, y0, x1, y1,
      is_depth ? xenos::GetDepthRenderTargetFormatName(
                     xenos::DepthRenderTargetFormat(depth_edram_info.format))
               : xenos::GetColorRenderTargetFormatName(
                     xenos::ColorRenderTargetFormat(color_edram_info.format)),
      dest_format_info.name, rb_copy_dest_base, copy_dest_extent_start, copy_dest_extent_end);

  return true;
}

ResolveCopyShaderIndex ResolveInfo::GetCopyShader(uint32_t draw_resolution_scale_x,
                                                  uint32_t draw_resolution_scale_y,
                                                  ResolveCopyShaderConstants& constants_out,
                                                  uint32_t& group_count_x_out,
                                                  uint32_t& group_count_y_out) const {
  ResolveCopyShaderIndex shader = ResolveCopyShaderIndex::kUnknown;
  bool is_depth = IsCopyingDepth();
  ResolveEdramInfo edram_info = is_depth ? depth_edram_info : color_edram_info;
  bool source_is_64bpp = !is_depth && color_edram_info.format_is_64bpp != 0;
  if (is_depth ||
      (!copy_dest_info.copy_dest_exp_bias &&
       xenos::IsSingleCopySampleSelected(copy_dest_coordinate_info.copy_sample_select) &&
       xenos::IsColorResolveFormatBitwiseEquivalent(
           xenos::ColorRenderTargetFormat(color_edram_info.format),
           xenos::ColorFormat(copy_dest_info.copy_dest_format)))) {
    if (edram_info.msaa_samples >= xenos::MsaaSamples::k4X) {
      shader = source_is_64bpp ? ResolveCopyShaderIndex::kFast64bpp4xMSAA
                               : ResolveCopyShaderIndex::kFast32bpp4xMSAA;
    } else {
      shader = source_is_64bpp ? ResolveCopyShaderIndex::kFast64bpp1x2xMSAA
                               : ResolveCopyShaderIndex::kFast32bpp1x2xMSAA;
    }
  } else {
    const FormatInfo& dest_format_info =
        *FormatInfo::Get(xenos::TextureFormat(copy_dest_info.copy_dest_format));
    switch (dest_format_info.bits_per_pixel) {
      case 8:
        shader = ResolveCopyShaderIndex::kFull8bpp;
        break;
      case 16:
        shader = ResolveCopyShaderIndex::kFull16bpp;
        break;
      case 32:
        shader = ResolveCopyShaderIndex::kFull32bpp;
        break;
      case 64:
        shader = ResolveCopyShaderIndex::kFull64bpp;
        break;
      case 128:
        shader = ResolveCopyShaderIndex::kFull128bpp;
        break;
      default:
        assert_unhandled_case(dest_format_info.bits_per_pixel);
    }
  }

  constants_out.dest_relative.edram_info = edram_info;
  constants_out.dest_relative.coordinate_info = coordinate_info;
  constants_out.dest_relative.dest_info = copy_dest_info;
  constants_out.dest_relative.dest_coordinate_info = copy_dest_coordinate_info;
  constants_out.dest_base = copy_dest_base;

  if (shader != ResolveCopyShaderIndex::kUnknown) {
    uint32_t width = (coordinate_info.width_div_8 << xenos::kResolveAlignmentPixelsLog2) *
                     draw_resolution_scale_x;
    uint32_t height =
        (height_div_8 << xenos::kResolveAlignmentPixelsLog2) * draw_resolution_scale_y;
    const ResolveCopyShaderInfo& shader_info = resolve_copy_shader_info[size_t(shader)];
    group_count_x_out =
        (width + ((1 << shader_info.group_size_x_log2) - 1)) >> shader_info.group_size_x_log2;
    group_count_y_out =
        (height + ((1 << shader_info.group_size_y_log2) - 1)) >> shader_info.group_size_y_log2;
  } else {
    REXGPU_ERROR("No resolve copy compute shader for the provided configuration");
    assert_always();
    group_count_x_out = 0;
    group_count_y_out = 0;
  }

  return shader;
}

}  // namespace renut::gpu::draw_util
