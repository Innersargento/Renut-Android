#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    reNut - taken into the app (renut::gpu::draw_util), without the Shader-dependent
 *              helpers and the host viewport code
 */

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include "gpu/core/draw_state.h"
#include "gpu/core/state_bits.h"
#include <rex/graphics/xenos.h>
#include <rex/memory.h>

namespace renut::gpu::draw_util {
using namespace rex::graphics;
namespace memory = rex::memory;


constexpr bool IsPrimitiveLine(bool vgt_output_path_is_tessellation_enable,
                               xenos::PrimitiveType type) {
  if (vgt_output_path_is_tessellation_enable && type == xenos::PrimitiveType::kLinePatch) {
    // For patch primitive types, the major mode is always explicit, so just
    // checking if VGT_OUTPUT_PATH_CNTL::path_select is kTessellationEnable is
    // enough.
    return true;
  }
  switch (type) {
    case xenos::PrimitiveType::kLineList:
    case xenos::PrimitiveType::kLineStrip:
    case xenos::PrimitiveType::kLineLoop:
    case xenos::PrimitiveType::k2DLineStrip:
      return true;
    default:
      break;
  }
  return false;
}

inline bool IsPrimitiveLine(const DrawState& st) {
  return IsPrimitiveLine(st.output_path_cntl.path_select ==
                             xenos::VGTOutputPath::kTessellationEnable,
                         st.draw_initiator.prim_type);
}

// Polygonal primitive types (not including points and lines) are rasterized as
// triangles, have front and back faces, and also support face culling and fill
// modes (polymode_front_ptype, polymode_back_ptype). Other primitive types are
// always "front" (but don't support front face and back face culling, according
// to OpenGL and Vulkan specifications - even if glCullFace is
// GL_FRONT_AND_BACK, points and lines are still drawn), and may in some cases
// use the "para" registers instead of "front" or "back" (for "parallelogram" -
// like poly_offset_para_enable).
constexpr bool IsPrimitivePolygonal(bool vgt_output_path_is_tessellation_enable,
                                    xenos::PrimitiveType type) {
  if (vgt_output_path_is_tessellation_enable &&
      (type == xenos::PrimitiveType::kTrianglePatch || type == xenos::PrimitiveType::kQuadPatch)) {
    // For patch primitive types, the major mode is always explicit, so just
    // checking if VGT_OUTPUT_PATH_CNTL::path_select is kTessellationEnable is
    // enough.
    return true;
  }
  switch (type) {
    case xenos::PrimitiveType::kTriangleList:
    case xenos::PrimitiveType::kTriangleFan:
    case xenos::PrimitiveType::kTriangleStrip:
    case xenos::PrimitiveType::kTriangleWithWFlags:
    case xenos::PrimitiveType::kQuadList:
    case xenos::PrimitiveType::kQuadStrip:
    case xenos::PrimitiveType::kPolygon:
      return true;
    default:
      break;
  }
  // TODO(Triang3l): Investigate how kRectangleList should be treated - possibly
  // actually drawn as two polygons on the console, however, the current
  // geometry shader doesn't care about the winding order - allowing backface
  // culling for rectangles currently breaks 4D53082D.
  return false;
}

inline bool IsPrimitivePolygonal(const DrawState& st) {
  return IsPrimitivePolygonal(st.output_path_cntl.path_select ==
                                  xenos::VGTOutputPath::kTessellationEnable,
                              st.draw_initiator.prim_type);
}

// Direct3D 10.1+ standard sample positions, also used in Vulkan, for
// calculations related to host MSAA, in 1/16th of a pixel.
extern const int8_t kD3D10StandardSamplePositions2x[2][2];
extern const int8_t kD3D10StandardSamplePositions4x[4][2];

bits::RB_DEPTHCONTROL GetNormalizedDepthControl(const DrawState& st, xenos::EdramMode edram_mode);

// Direct3D 9 and Xenos constant polygon offset is an absolute floating-point
// value.
// It's possibly treated just as an absolute offset by the Xenos too - the
// PA_SU_POLY_OFFSET_DB_FMT_CNTL::POLY_OFFSET_DB_IS_FLOAT_FMT switch was added
// later in the R6xx, though this needs verification.
// 5454082B, for example, for float24, sets the bias to 0.000002 - or slightly
// above 2^-19.
// The total polygon offset formula specified by Direct3D 9 is:
// `offset = slope * slope factor + constant offset`
//
// Direct3D 10, Metal, OpenGL and Vulkan, however, take the constant polygon
// offset factor as a relative value, with the formula being:
// `offset = slope * slope factor +
//           maximum resolvable difference * constant factor`
// where the maximum resolvable difference is:
// - For a fixed-point depth buffer, the minimum representable non-zero value in
//   the depth buffer, that is 1 / (2^24 - 1) for unorm24 (on Vulkan though it's
//   allowed to be up to 2 / 2^24 in this case).
// - For a floating-point depth buffer, it's:
//   2 ^ (exponent of the maximum Z in the primitive - the number of explicitly
//        stored mantissa bits)
//   (23 explicitly stored bits for float32 - and 20 explicitly stored bits for
//    float24).
//
// While the polygon offset is a fixed-function feature in the pipeline, and the
// formula can't be toggled between absolute and relative, it's important that
// Xenia translates the guest absolute depth bias into the host relative depth
// bias in a way that the values that separate coplanar geometry on the guest
// qualitatively also still correctly separate them on the host.
//
// It also should be taken into account that on Xenia, float32 depth values may
// be snapped to float24 directly in the translated pixel shaders (to prevent
// data loss if after reuploading a depth buffer to the EDRAM there's no way to
// recover the full-precision value, that results in the inability to perform
// more rendering passes for the same geometry), and not only to the nearest
// value, but also just truncating them (so in case of data loss, the "greater
// or equal" depth test function still works).
//
// Because of this, the depth bias may be lost if Xenia translates it into a too
// small value, and the conversion of the depth is done in the pixel shader.
// Specifically, Xenia should not simply convert a value that separates coplanar
// primitives as float24 just into something that still separates them as
// float32. Essentially, if conversion to float24 is done in the pixel shader,
// Xenia should make sure the polygon offset on the host is calculated as if the
// host had float24 depth too, not float32.

// Applies to both native host unorm24, and unorm24 emulated as host float32.
// For native unorm24, this is exactly the inverse of the minimum representable
// non-zero value.
// For unorm24 emulated as float32, the minimum representable non-zero value for
// a primitive in the [0.5, 1) range (the worst case that forward depth reaches
// very quickly, at nearly `2 * near clipping plane distance`) is 2 ^ (-1 - 23),
// or 2^-24, and this factor is almost 2^24.
constexpr float kD3D10PolygonOffsetFactorUnorm24 = float((UINT32_C(1) << 24) - 1);

// For a host floating-point depth buffer, the integer value of the depth bias
// is roughly how many ULPs primitives should be separated by.
//
// Float24, however, has 3 mantissa bits fewer than float32 - so one float24 ULP
// corresponds to 8 float32 ULPs - which means each conceptual "layer" of the
// guest value should correspond to a polygon offset of 8. So, after the guest
// absolute value is converted to "layers", it should be multiplied by 8 before
// being used on the host with a float32 depth buffer.
//
// The scale for converting the guest absolute depth bias to the "layers" needs
// to be determined for the worst case - specifically, the [0.5, 1) range (1 is
// a single value, so there's no need to take it into consideration). In this
// range, Z values have the exponent of -1. Therefore, for float24, the absolute
// offset is obtained from the "layer index" in this range as (disregarding the
// slope term):
// offset = 2 ^ (-1 - 20) * constant factor
// Thus, to obtain the constant factor from the absolute offset in the range
// with the lowest absolute precision, the offset needs to be multiplied by
// 2^21.
//
// Finally, the 0...0.5 range may be used on the host to represent the 0...1
// guest depth range to be able to copy all possible encodings, which are
// [0, 2), via a [0, 1] depth output variable, during EDRAM contents
// reinterpretation. This is done by scaling the viewport depth bounds by 0.5.
// However, there's no need to do anything to handle this scenario in the
// polygon offset - it's calculated after applying the viewport transformation,
// and the maximum Z value in the primitive will have an exponent lowered by 1,
// thus the result will also have an exponent lowered by 1 - exactly what's
// needed for remapping 0...1 to 0...0.5.
constexpr float kD3D10PolygonOffsetFactorFloat24 = float(UINT32_C(1) << (21 + 3));

// For hosts not supporting separate front and back polygon offsets, returns the
// polygon offset for the face which likely needs the offset the most (and that
// will not be culled). The values returned will have the units of the original
// registers (the scale is for 1/16 subpixels, multiply by
// xenos::kPolygonOffsetScaleSubpixelUnit outside if the value for pixels is
// needed).
void GetPreferredFacePolygonOffset(const DrawState& st, bool primitive_polygonal,
                                   float& scale_out, float& offset_out);

struct Scissor {
  // Offset from render target UV = 0 to +UV.
  uint32_t offset[2];
  // Extent can be zero.
  uint32_t extent[2];
};
// `surface_pitch`: the pitch of the bound surface, in pixels, which the scissor is held inside; 0 for none.
void GetScissor(const DrawState& st, Scissor& scissor_out, uint32_t surface_pitch = 0);

}  // namespace renut::gpu::draw_util
