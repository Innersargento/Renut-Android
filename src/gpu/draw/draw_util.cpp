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
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include "gpu/xenos/registers.h"
#include "gpu/draw/draw_util.h"
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/ui/graphics_util.h>

namespace renut::gpu::draw_util {
using namespace rex::graphics;
namespace memory = rex::memory;
namespace ui = rex::ui;


reg::RB_DEPTHCONTROL GetNormalizedDepthControl(const RegisterFile& regs, xenos::EdramMode edram_mode) {
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

void GetScissor(const RegisterFile& regs, Scissor& scissor_out, uint32_t surface_pitch) {
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
  if (surface_pitch) {
    // Clamp the horizontal scissor to the surface's pitch for safety, in case that's
    // not done by the guest for some reason (it's not when doing draws without
    // clipping in Direct3D 9, for instance), to prevent overflow.
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

}  // namespace renut::gpu::draw_util
