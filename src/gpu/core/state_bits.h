/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    reNut - taken into the app (renut::gpu); the register indices are gone, what is
 *              left is the layout of the bits of the words the game's Direct3D device keeps
 */

#pragma once

// The bit layout of the state words in the game's Direct3D device (blend, depth, raster, the
// surfaces, the copy). The device keeps them in the Xenos register format, so the format stays;
// the register file does not - DrawState (draw_state.h) holds them by name.
//
// All unused bits are intentionally declared as named fields for stable comparisons when
// values are constructed or modified by the backend itself.
//
// Only 32-bit types (uint32_t, int32_t, float or enums with uint32_t / int32_t as the
// underlying type) are allowed in the bit fields here, as Visual C++ restarts packing when a
// field requires different alignment than the previous one.

#include <cstdint>
#include <cstdlib>

#include <rex/assert.h>
#include <rex/graphics/xenos.h>

namespace renut::gpu {
namespace xenos = rex::graphics::xenos;


namespace bits {

/*******************************************************************************
   ___ ___  _  _ _____ ___  ___  _
  / __/ _ \| \| |_   _| _ \/ _ \| |
 | (_| (_) | .` | | | |   / (_) | |__
  \___\___/|_|\_| |_| |_|_\\___/|____|

*******************************************************************************/

/*******************************************************************************
  ___ ___ ___  _   _ ___ _  _  ___ ___ ___
 / __| __/ _ \| | | | __| \| |/ __| __| _ \
 \__ \ _| (_) | |_| | _|| .` | (__| _||   /
 |___/___\__\_\\___/|___|_|\_|\___|___|_|_\

*******************************************************************************/

union alignas(uint32_t) SQ_VS_CONST {
  uint32_t value;
  struct {
    uint32_t base : 9;    // +0
    uint32_t _pad_9 : 3;  // +9
    // Vec4 count minus one.
    uint32_t size : 9;      // +12, value minus 1
    uint32_t _pad_21 : 11;  // +21
  };
};
static_assert_size(SQ_VS_CONST, sizeof(uint32_t));

// Same as SQ_VS_CONST.
/*******************************************************************************
 __   _____ ___ _____ _____  __
 \ \ / / __| _ \_   _| __\ \/ /
  \ V /| _||   / | | | _| >  <
   \_/ |___|_|_\ |_| |___/_/\_\

   ___ ___  ___  _   _ ___ ___ ___     _   _  _ ___
  / __| _ \/ _ \| | | | _ \ __| _ \   /_\ | \| |   \
 | (_ |   / (_) | |_| |  _/ _||   /  / _ \| .` | |) |
  \___|_|_\\___/ \___/|_| |___|_|_\ /_/ \_\_|\_|___/

  _____ ___ ___ ___ ___ _    _      _ _____ ___  ___
 |_   _| __/ __/ __| __| |  | |    /_\_   _/ _ \| _ \
   | | | _|\__ \__ \ _|| |__| |__ / _ \| || (_) |   /
   |_| |___|___/___/___|____|____/_/ \_\_| \___/|_|_\

*******************************************************************************/

union alignas(uint32_t) VGT_DMA_SIZE {
  uint32_t value;
  struct {
    uint32_t num_words : 24;      // +0
    uint32_t _pad_24 : 6;         // +24
    xenos::Endian swap_mode : 2;  // +30
  };
};

union alignas(uint32_t) VGT_DRAW_INITIATOR {
  uint32_t value;
  // Has differences from the Adreno 200.
  struct {
    xenos::PrimitiveType prim_type : 6;     // +0
    xenos::SourceSelect source_select : 2;  // +6
    // Adreno 200 replaced this with FACENESS_CULL_SELECT possibly due to the
    // removal of tessellation, but on the Xenos this is MAJOR_MODE like on the
    // R600, it's set to the explicit mode mainly for tessellated draws in games
    // (because VGT_OUTPUT_PATH_CNTL where tessellation is enabled is ignored in
    // the implicit major mode).
    xenos::MajorMode major_mode : 2;    // +8
    uint32_t _pad_10 : 1;               // +10
    xenos::IndexFormat index_size : 1;  // +11
    uint32_t not_eop : 1;               // +12
    uint32_t _pad_13 : 3;               // +13
    uint32_t num_indices : 16;          // +16
  };
};
static_assert_size(VGT_DRAW_INITIATOR, sizeof(uint32_t));

// Unlike on R6xx (but closer to R5xx), and according to the Adreno 200 header,
// the registers related to the vertex index are 24-bit. Vertex indices are
// unsigned, and only the lower 24 bits of them are actually used by the GPU -
// this has been verified on an Adreno 200 phone (LG Optimus L7) on OpenGL ES
// using a GL_UNSIGNED_INT element array buffer with junk in the upper 8 bits
// that had no effect on drawing.

// The order of operations is primitive reset index checking -> offsetting ->
// clamping.

union alignas(uint32_t) VGT_OUTPUT_PATH_CNTL {
  uint32_t value;
  struct {
    xenos::VGTOutputPath path_select : 2;  // +0
    uint32_t _pad_2 : 30;                  // +2
  };
};
static_assert_size(VGT_OUTPUT_PATH_CNTL, sizeof(uint32_t));

union alignas(uint32_t) VGT_HOS_CNTL {
  uint32_t value;
  struct {
    xenos::TessellationMode tess_mode : 2;  // +0
    uint32_t _pad_2 : 30;                   // +2
  };
};
static_assert_size(VGT_HOS_CNTL, sizeof(uint32_t));

/*******************************************************************************
  ___ ___ ___ __  __ ___ _____ _____   _____
 | _ \ _ \_ _|  \/  |_ _|_   _|_ _\ \ / / __|
 |  _/   /| || |\/| || |  | |  | | \ V /| _|
 |_| |_|_\___|_|  |_|___| |_| |___| \_/ |___|

    _   ___ ___ ___ __  __ ___ _    ___ ___
   /_\ / __/ __| __|  \/  | _ ) |  | __| _ \
  / _ \\__ \__ \ _|| |\/| | _ \ |__| _||   /
 /_/ \_\___/___/___|_|  |_|___/____|___|_|_\

*******************************************************************************/

union alignas(uint32_t) PA_SU_POINT_MINMAX {
  uint32_t value;
  struct {
    // For per-vertex size specification, radius (1/2 size), 12.4 fixed point.
    uint32_t min_size : 16;  // +0
    uint32_t max_size : 16;  // +16
  };
};
static_assert_size(PA_SU_POINT_MINMAX, sizeof(uint32_t));

union alignas(uint32_t) PA_SU_POINT_SIZE {
  uint32_t value;
  struct {
    // 1/2 width or height, 12.4 fixed point.
    uint32_t height : 16;  // +0
    uint32_t width : 16;   // +16
  };
};
static_assert_size(PA_SU_POINT_SIZE, sizeof(uint32_t));

// Setup Unit / Scanline Converter mode cntl
union alignas(uint32_t) PA_SU_SC_MODE_CNTL {
  uint32_t value;
  struct {
    uint32_t cull_front : 1;  // +0
    uint32_t cull_back : 1;   // +1
    // 0 - front is CCW, 1 - front is CW.
    uint32_t face : 1;  // +2
    // 4541096E uses poly_mode 2 for triangles, which is "reserved" on R6xx and
    // not defined on Adreno 2xx, but polymode_front/back_ptype are 0 (points)
    // in this case in 4541096E, which should not be respected for non-kDualMode
    // as the title wants to draw filled triangles.
    xenos::PolygonModeEnable poly_mode : 2;       // +3
    xenos::PolygonType polymode_front_ptype : 3;  // +5
    xenos::PolygonType polymode_back_ptype : 3;   // +8
    uint32_t poly_offset_front_enable : 1;        // +11
    uint32_t poly_offset_back_enable : 1;         // +12
    uint32_t poly_offset_para_enable : 1;         // +13
    uint32_t _pad_14 : 1;                         // +14
    uint32_t msaa_enable : 1;                     // +15
    uint32_t vtx_window_offset_enable : 1;        // +16
    // LINE_STIPPLE_ENABLE was added on Adreno.
    uint32_t _pad_17 : 2;                // +17
    uint32_t provoking_vtx_last : 1;     // +19
    uint32_t persp_corr_dis : 1;         // +20
    uint32_t multi_prim_ib_ena : 1;      // +21
    uint32_t _pad_22 : 1;                // +22
    uint32_t quad_order_enable : 1;      // +23
    uint32_t sc_one_quad_per_clock : 1;  // +24
    // WAIT_RB_IDLE_ALL_TRI and WAIT_RB_IDLE_FIRST_TRI_NEW_STATE were added on
    // Adreno.
    uint32_t _pad_25 : 7;  // +25
  };
};
static_assert_size(PA_SU_SC_MODE_CNTL, sizeof(uint32_t));

// Setup Unit Vertex Control
union alignas(uint32_t) PA_SU_VTX_CNTL {
  uint32_t value;
  struct {
    xenos::PixelCenter pix_center : 1;         // +0
    xenos::VertexRounding round_mode : 2;      // +1
    xenos::VertexQuantization quant_mode : 3;  // +3
    uint32_t _pad_6 : 26;                      // +6
  };
};
static_assert_size(PA_SU_VTX_CNTL, sizeof(uint32_t));

// Scanline converter viz query, used by D3D for gpu side conditional rendering
union alignas(uint32_t) PA_SC_VIZ_QUERY {
  uint32_t value;
  struct {
    // the visibility of draws should be evaluated
    uint32_t viz_query_ena : 1;  // +0
    uint32_t viz_query_id : 6;   // +1
    // discard geometry after test (but use for testing)
    uint32_t kill_pix_post_hi_z : 1;  // +7
    // not used with d3d
    uint32_t kill_pix_post_detail_mask : 1;  // +8
    uint32_t _pad_9 : 23;                    // +9
  };
};
static_assert_size(PA_SC_VIZ_QUERY, sizeof(uint32_t));

// Clipper clip control
union alignas(uint32_t) PA_CL_CLIP_CNTL {
  uint32_t value;
  struct {
    // Like on the Adreno 200, but with user clip planes from R3xx (used in
    // 4D5307E6 for the hanging lamp on Last Resort).
    uint32_t ucp_ena_0 : 1;               // +0
    uint32_t ucp_ena_1 : 1;               // +1
    uint32_t ucp_ena_2 : 1;               // +2
    uint32_t ucp_ena_3 : 1;               // +3
    uint32_t ucp_ena_4 : 1;               // +4
    uint32_t ucp_ena_5 : 1;               // +5
    uint32_t _pad_6 : 8;                  // +6
    uint32_t ps_ucp_mode : 2;             // +14
    uint32_t clip_disable : 1;            // +16
    uint32_t ucp_cull_only_ena : 1;       // +17
    uint32_t boundary_edge_flag_ena : 1;  // +18
    uint32_t dx_clip_space_def : 1;       // +19
    uint32_t dis_clip_err_detect : 1;     // +20
    uint32_t vtx_kill_or : 1;             // +21
    uint32_t xy_nan_retain : 1;           // +22
    uint32_t z_nan_retain : 1;            // +23
    uint32_t w_nan_retain : 1;            // +24
    uint32_t _pad_25 : 7;                 // +25
  };
  struct {
    uint32_t ucp_ena : 6;
  };
};
static_assert_size(PA_CL_CLIP_CNTL, sizeof(uint32_t));

// Viewport transform engine control
union alignas(uint32_t) PA_CL_VTE_CNTL {
  uint32_t value;
  struct {
    uint32_t vport_x_scale_ena : 1;   // +0
    uint32_t vport_x_offset_ena : 1;  // +1
    uint32_t vport_y_scale_ena : 1;   // +2
    uint32_t vport_y_offset_ena : 1;  // +3
    uint32_t vport_z_scale_ena : 1;   // +4
    uint32_t vport_z_offset_ena : 1;  // +5
    uint32_t _pad_6 : 2;              // +6
    uint32_t vtx_xy_fmt : 1;          // +8
    uint32_t vtx_z_fmt : 1;           // +9
    uint32_t vtx_w0_fmt : 1;          // +10
    uint32_t perfcounter_ref : 1;     // +11
    uint32_t _pad_12 : 20;            // +12
  };
};
static_assert_size(PA_CL_VTE_CNTL, sizeof(uint32_t));

union alignas(uint32_t) PA_SC_SCREEN_SCISSOR_TL {
  uint32_t value;
  struct {
    int32_t tl_x : 15;     // +0
    uint32_t _pad_15 : 1;  // +15
    int32_t tl_y : 15;     // +16
    uint32_t _pad_31 : 1;  // +31
  };
};
static_assert_size(PA_SC_SCREEN_SCISSOR_TL, sizeof(uint32_t));

union alignas(uint32_t) PA_SC_SCREEN_SCISSOR_BR {
  uint32_t value;
  struct {
    int32_t br_x : 15;     // +0
    uint32_t _pad_15 : 1;  // +15
    int32_t br_y : 15;     // +16
    uint32_t _pad_31 : 1;  // +31
  };
};
static_assert_size(PA_SC_SCREEN_SCISSOR_BR, sizeof(uint32_t));

union alignas(uint32_t) PA_SC_WINDOW_OFFSET {
  uint32_t value;
  struct {
    int32_t window_x_offset : 15;  // +0
    uint32_t _pad_15 : 1;          // +15
    int32_t window_y_offset : 15;  // +16
    uint32_t _pad_31 : 1;          // +31
  };
};
static_assert_size(PA_SC_WINDOW_OFFSET, sizeof(uint32_t));

union alignas(uint32_t) PA_SC_WINDOW_SCISSOR_TL {
  uint32_t value;
  struct {
    uint32_t tl_x : 14;                  // +0
    uint32_t _pad_14 : 2;                // +14
    uint32_t tl_y : 14;                  // +16
    uint32_t _pad_30 : 1;                // +30
    uint32_t window_offset_disable : 1;  // +31
  };
};
static_assert_size(PA_SC_WINDOW_SCISSOR_TL, sizeof(uint32_t));

union alignas(uint32_t) PA_SC_WINDOW_SCISSOR_BR {
  uint32_t value;
  struct {
    uint32_t br_x : 14;    // +0
    uint32_t _pad_14 : 2;  // +14
    uint32_t br_y : 14;    // +16
    uint32_t _pad_30 : 2;  // +30
  };
};
static_assert_size(PA_SC_WINDOW_SCISSOR_BR, sizeof(uint32_t));

/*******************************************************************************
  ___ ___ _  _ ___  ___ ___
 | _ \ __| \| |   \| __| _ \
 |   / _|| .` | |) | _||   /
 |_|_\___|_|\_|___/|___|_|_\

  ___   _   ___ _  _____ _  _ ___
 | _ ) /_\ / __| |/ / __| \| |   \
 | _ \/ _ \ (__| ' <| _|| .` | |) |
 |___/_/ \_\___|_|\_\___|_|\_|___/

*******************************************************************************/

union alignas(uint32_t) RB_MODECONTROL {
  uint32_t value;
  struct {
    xenos::EdramMode edram_mode : 3;  // +0
    uint32_t _pad_3 : 29;             // +3
  };
};
static_assert_size(RB_MODECONTROL, sizeof(uint32_t));

union alignas(uint32_t) RB_SURFACE_INFO {
  uint32_t value;
  struct {
    uint32_t surface_pitch : 14;          // +0 in pixels.
    uint32_t _pad_14 : 2;                 // +14
    xenos::MsaaSamples msaa_samples : 2;  // +16
    uint32_t hiz_pitch : 14;              // +18
  };
};
static_assert_size(RB_SURFACE_INFO, sizeof(uint32_t));

union alignas(uint32_t) RB_COLORCONTROL {
  uint32_t value;
  struct {
    xenos::CompareFunction alpha_func : 3;  // +0
    uint32_t alpha_test_enable : 1;         // +3
    uint32_t alpha_to_mask_enable : 1;      // +4
    // Everything in between was added on Adreno.
    uint32_t _pad_5 : 19;  // +5
    // TODO(Triang3l): Redo these tests and possibly flip these vertically in
    // the comment and in the actual implementation. It appears that
    // gl_FragCoord.y is mirrored as opposed to the actual screen coordinates in
    // the rasterizer (see the SQ_CONTEXT_MISC::param_gen_pos comment here).
    // According to tests on an Adreno 200 device (LG Optimus L7), done by
    // drawing 0.5x0.5 rectangles in different corners of four pixels in a quad
    // to a multisampled GLSurfaceView, the coverage mask is the following for 4
    // samples:
    // 0.25)  [0.25, 0.5)  [0.5, 0.75)  [0.75, 1)   [1
    //  --        --           --          --       --
    // |  |      |  |         | #|        |##|     |##|
    // |  |      |# |         |# |        |# |     |##|
    //  --        --           --          --       --
    // (gl_FragCoord.y near 0 in the top, near 1 in the bottom here - D3D-like.)
    // For 2 samples, the top sample (closer to gl_FragCoord.y 0) is covered
    // when alpha is in [0.5, 1), the bottom sample is covered when the alpha is
    // [1. With these thresholds, however, in 5454082B, almost all distant trees
    // are transparent, this is asymmetric - fully transparent for a quarter of
    // the range (or even half of the range for 2x and almost the entire range
    // for 1x), but fully opaque only in one value.
    // Though, 2, 2, 2, 2 offset values are commonly used for undithered alpha
    // to coverage (in games such as 5454082B, and overall in AMD driver
    // implementations) - it appears that 2, 2, 2, 2 offsets are supposed to
    // make this symmetric.
    // Both 5454082B and RADV (which used AMDVLK as a reference) use 3, 1, 0, 2
    // offsets for dithered alpha to mask.
    // https://gitlab.freedesktop.org/nchery/mesa/commit/8a52e4cc4fad4f1c75acc0badd624778f9dfe202
    // It appears that the offsets lower the thresholds by (offset / 4 /
    // sample count). That's consistent with both 2, 2, 2, 2 making the test
    // symmetric and 0, 0, 0, 0 (forgetting to set the offset values) resulting
    // in what the official Adreno 200 driver for Android (which is pretty buggy
    // overall) produces.
    // According to Evergreen register reference:
    // - offset0 is for pixel (0, 0) in each quad.
    // - offset1 is for pixel (0, 1) in each quad.
    // - offset2 is for pixel (1, 0) in each quad.
    // - offset3 is for pixel (1, 1) in each quad.
    uint32_t alpha_to_mask_offset0 : 2;  // +24
    uint32_t alpha_to_mask_offset1 : 2;  // +26
    uint32_t alpha_to_mask_offset2 : 2;  // +28
    uint32_t alpha_to_mask_offset3 : 2;  // +30
  };
};
static_assert_size(RB_COLORCONTROL, sizeof(uint32_t));

union alignas(uint32_t) RB_COLOR_INFO {
  uint32_t value;
  struct {
    // The original R400 structure has 12-bit color_base, however the Xenos has
    // periodic 11-bit EDRAM tile addressing, so this field was split in Xenia
    // for convenience and to avoid mistakes.
    uint32_t color_base : 11;                         // +0 in tiles.
    uint32_t color_base_bit_11 : 1;                   // +11
    uint32_t _pad_12 : 4;                             // +12
    xenos::ColorRenderTargetFormat color_format : 4;  // +16
    int32_t color_exp_bias : 6;                       // +20
    uint32_t _pad_26 : 6;                             // +26
  };
  // RB_COLOR[1-3]_INFO also use this format.
};
static_assert_size(RB_COLOR_INFO, sizeof(uint32_t));

union alignas(uint32_t) RB_COLOR_MASK {
  uint32_t value;
  struct {
    uint32_t write_red0 : 1;    // +0
    uint32_t write_green0 : 1;  // +1
    uint32_t write_blue0 : 1;   // +2
    uint32_t write_alpha0 : 1;  // +3
    uint32_t write_red1 : 1;    // +4
    uint32_t write_green1 : 1;  // +5
    uint32_t write_blue1 : 1;   // +6
    uint32_t write_alpha1 : 1;  // +7
    uint32_t write_red2 : 1;    // +8
    uint32_t write_green2 : 1;  // +9
    uint32_t write_blue2 : 1;   // +10
    uint32_t write_alpha2 : 1;  // +11
    uint32_t write_red3 : 1;    // +12
    uint32_t write_green3 : 1;  // +13
    uint32_t write_blue3 : 1;   // +14
    uint32_t write_alpha3 : 1;  // +15
    uint32_t _pad_16 : 16;      // +16
  };
};
static_assert_size(RB_COLOR_MASK, sizeof(uint32_t));

union alignas(uint32_t) RB_BLENDCONTROL {
  uint32_t value;
  struct {
    xenos::BlendFactor color_srcblend : 5;   // +0
    xenos::BlendOp color_comb_fcn : 3;       // +5
    xenos::BlendFactor color_destblend : 5;  // +8
    uint32_t _pad_13 : 3;                    // +13
    xenos::BlendFactor alpha_srcblend : 5;   // +16
    xenos::BlendOp alpha_comb_fcn : 3;       // +21
    xenos::BlendFactor alpha_destblend : 5;  // +24
    // BLEND_FORCE_ENABLE and BLEND_FORCE were added on Adreno.
    uint32_t _pad_29 : 3;  // +29
  };
  // RB_BLENDCONTROL[0-3] use this format.
};
static_assert_size(RB_BLENDCONTROL, sizeof(uint32_t));

union alignas(uint32_t) RB_DEPTHCONTROL {
  uint32_t value;
  struct {
    uint32_t stencil_enable : 1;  // +0
    uint32_t z_enable : 1;        // +1
    uint32_t z_write_enable : 1;  // +2
    // EARLY_Z_ENABLE was added on Adreno, never set by Xbox 360 games.
    uint32_t _pad_3 : 1;                        // +3
    xenos::CompareFunction zfunc : 3;           // +4
    uint32_t backface_enable : 1;               // +7
    xenos::CompareFunction stencilfunc : 3;     // +8
    xenos::StencilOp stencilfail : 3;           // +11
    xenos::StencilOp stencilzpass : 3;          // +14
    xenos::StencilOp stencilzfail : 3;          // +17
    xenos::CompareFunction stencilfunc_bf : 3;  // +20
    xenos::StencilOp stencilfail_bf : 3;        // +23
    xenos::StencilOp stencilzpass_bf : 3;       // +26
    xenos::StencilOp stencilzfail_bf : 3;       // +29
  };
};
static_assert_size(RB_DEPTHCONTROL, sizeof(uint32_t));

union alignas(uint32_t) RB_STENCILREFMASK {
  uint32_t value;
  struct {
    uint32_t stencilref : 8;        // +0
    uint32_t stencilmask : 8;       // +8
    uint32_t stencilwritemask : 8;  // +16
    uint32_t _pad_24 : 8;           // +24
  };
  // RB_STENCILREFMASK_BF also uses this format.
};
static_assert_size(RB_STENCILREFMASK, sizeof(uint32_t));

union alignas(uint32_t) RB_DEPTH_INFO {
  uint32_t value;
  struct {
    // The original R400 structure has 12-bit depth_base, however the Xenos has
    // periodic 11-bit EDRAM tile addressing, so this field was split in Xenia
    // for convenience and to avoid mistakes.
    uint32_t depth_base : 11;                         // +0 in tiles.
    uint32_t depth_base_bit_11 : 1;                   // +11
    uint32_t _pad_12 : 4;                             // +12
    xenos::DepthRenderTargetFormat depth_format : 1;  // +16
    uint32_t _pad_17 : 15;                            // +17
  };
};
static_assert_size(RB_DEPTH_INFO, sizeof(uint32_t));

// Copy registers are very different than on Adreno.

union alignas(uint32_t) RB_COPY_CONTROL {
  uint32_t value;
  struct {
    uint32_t copy_src_select : 3;                    // +0 Depth is 4.
    uint32_t _pad_3 : 1;                             // +3
    xenos::CopySampleSelect copy_sample_select : 3;  // +4
    uint32_t _pad_7 : 1;                             // +7
    uint32_t color_clear_enable : 1;                 // +8
    uint32_t depth_clear_enable : 1;                 // +9
    uint32_t _pad_10 : 10;                           // +10
    xenos::CopyCommand copy_command : 2;             // +20
    uint32_t _pad_22 : 10;                           // +22
  };
};
static_assert_size(RB_COPY_CONTROL, sizeof(uint32_t));

union alignas(uint32_t) RB_COPY_DEST_INFO {
  uint32_t value;
  struct {
    xenos::Endian128 copy_dest_endian : 3;            // +0
    uint32_t copy_dest_array : 1;                     // +3
    uint32_t copy_dest_slice : 3;                     // +4
    xenos::ColorFormat copy_dest_format : 6;          // +7
    xenos::SurfaceNumberFormat copy_dest_number : 3;  // +13
    int32_t copy_dest_exp_bias : 6;                   // +16
    uint32_t _pad_22 : 2;                             // +22
    uint32_t copy_dest_swap : 1;                      // +24
    uint32_t _pad_25 : 7;                             // +25
  };
};
static_assert_size(RB_COPY_DEST_INFO, sizeof(uint32_t));

/*******************************************************************************
  ___ ___ ___ ___ _      ___   __
 |   \_ _/ __| _ \ |    /_\ \ / /
 | |) | |\__ \  _/ |__ / _ \ V /
 |___/___|___/_| |____/_/ \_\_|

   ___ ___  _  _ _____ ___  ___  _    _    ___ ___
  / __/ _ \| \| |_   _| _ \/ _ \| |  | |  | __| _ \
 | (_| (_) | .` | | | |   / (_) | |__| |__| _||   /
  \___\___/|_|\_| |_| |_|_\\___/|____|____|___|_|_\

*******************************************************************************/

}  // namespace bits

}  // namespace renut::gpu
