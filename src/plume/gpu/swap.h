#pragma once
// GPU backend subsystem Swap. See backend.h.
#include "types.h"

namespace renut::gpu {

class GpuBackend;

struct Swap {
  explicit Swap(GpuBackend& owner) : backend(owner) {}
  GpuBackend& backend;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width, uint32_t frontbuffer_height);
  bool native_on_ = false;
  bool pred_pass_all_ = false;
  bool band_fragpos_ = false;
  bool resolve_log_ = false;
  uint32_t adaptive_mode_ = 1;
  uint32_t bc_mode_ = 1;
  // Debug: <files>/plume-no-persist sends every stream through the ring again.
  bool keep_streams_ = true;
  bool depth_alias_ = true;
  bool shader_cache_ = true;
  bool key_reuse_ = true;
  std::vector<TracedStream> traced_streams_;
  // Pixel shaders whose shadow-cascade constants this trace already holds.
  std::unordered_set<uint64_t> traced_cascade_ps_;
  // Armed trace, for effects too short to hit by request: while
  // <files>/plume-trace-arm exists, the shader pairs of 150 frames are learned,
  // and then a frame that draws a pair not seen before has the next 3 frames
  // traced into <files>/plume-trace-arm-<burst>-<frame>.txt (6 bursts at most).
  bool arm_ = false;
  uint32_t arm_learn_ = 0, arm_burst_ = 0, arm_left_ = 0;
  std::unordered_set<uint64_t> arm_pairs_;
  std::vector<std::pair<uint64_t, uint64_t>> arm_new_;
  // Where the traced frame's exported ranges go: "exportdump", or a folder of
  // its own for an armed frame.
  std::string trace_dump_name_ = "exportdump";
  bool shadow_native_constants_ = true;
  // What exporting shaders write stays on the GPU: the draws that read it are
  // served from the export buffers. Copying it back to guest memory a few
  // frames late overwrote memory the game had freed and reused (its asserts
  // fired, then it wrote through garbage pointers or fed the CP garbage), so
  // it is only done, for debugging, while <files>/plume-writeback exists.
  bool writeback_ = false;
  bool writeback_fast_ = true;
  // Debug: <files>/plume-drawskip holds "<first> <end>": draws of each frame
  // with an index in that range are dropped (to bisect a rendering bug).
  uint32_t drawskip_first_ = 0, drawskip_end_ = 0, frame_draw_ = 0;
  // Debug: <files>/plume-white-slots holds a hex mask of 2D fetch slots that
  // every draw samples as white - e.g. 8000 whitens slot 15 (a shadow map reads
  // as fully lit), to see which texture a lighting artifact comes from.
  uint32_t white_slots_ = 0;
  // Debug: <files>/plume-pixel-center holds 1 to drop the D3D9 half-pixel
  // shift from depth-only draws (the shadow maps), 2 from every draw.
  uint32_t pixel_center_debug_ = 0;
  // Debug: <files>/plume-snap holds "<hex address> <n>": the n-th resolve of
  // the frame into that address is copied aside and presented instead of the
  // front buffer, to see an intermediate pass on screen.
  uint32_t snap_address_ = 0, snap_ordinal_ = 0, snap_seen_ = 0;
  // The tallest a draw has rendered, which bounds what a clear may grow.
  std::chrono::steady_clock::time_point frame_start_{};
  uint64_t cpu_at_swap_ = 0;
  uint32_t snap_width_ = 0, snap_height_ = 0;
  RenderFormat snap_format_ = RenderFormat::UNKNOWN;
  bool snap_taken_ = false;
  // Debug: <files>/plume-snap-shader holds a pixel shader hash (hex). With
  // plume-snap-dump present too, the first colour resolve one of its draws
  // samples is snapshotted and dumped, then both files go - for a texture
  // whose resolve address changes every run, like the world portal's photo.
  uint64_t snap_shader_ = 0;
  // Debug: while <files>/plume-snap-dump exists, the next snapshot is also
  // read back into <files>/plume-snap.rgba (a text header, then the texels) -
  // the blit that presents it cannot show the alpha channel.
  std::unique_ptr<RenderBuffer> snap_dump_;
  uint32_t snap_dump_row_ = 0;
  std::vector<ConstOverride> const_overrides_;
  bool twin_dedupe_ = true;
  // <files>/plume-refractfix "0" turns off the refraction sprite bounds fix.
  bool refraction_fix_ = true;
  // The game's polygon offset (PA_SU_POLY_OFFSET_*): ground decals - footprints,
  // the tread blocks and skid marks a vehicle leaves - are quads lying on the
  // terrain and ask to be pulled a little toward the camera. Applying it made
  // no visible difference to those marks on open ground (measured 04/10, scales
  // 1 and 16), but on flat planks like the pier's they fight the surface and
  // flicker. Set each frame from renut_plume_poly_offset_percent (0 = off).
  bool poly_offset_ = true;
  float poly_offset_scale_ = 1.0f;
  // Debug: <files>/plume-depth-dump holds a pixel shader hash (hex). The first
  // depth resolve one of its draws samples is read back, as the shader sees it,
  // into <files>/plume-depth.f32 (a text header "width height row format
  // generation frame", then row-padded texels: R32G32_FLOAT is depth, stencil),
  // and the trigger goes. The splash's soft fade reads it and nothing else.
  uint64_t depth_dump_ps_ = 0;
  std::unique_ptr<RenderBuffer> depth_dump_;
  uint32_t depth_dump_w_ = 0, depth_dump_h_ = 0, depth_dump_row_ = 0, depth_dump_format_ = 0;
  uint64_t depth_dump_generation_ = 0, depth_dump_frame_ = 0;
  uint32_t pairprof_mode_ = 0, pairprof_frames_left_ = 0;
  // Whether each recorded draw leaves its entry in recent_draws_, for the report
  // of a lost device. A hash lookup, ~200 bytes of stream addresses and a few
  // buffer-address calls a draw: on by default only where the GPU is a Mali,
  // which has lost its device on a draw's data; files/plume-trail overrides.
  bool trail_ = false;
  bool skin_log_ = false;
  std::unordered_set<uint64_t> skin_skip_;
  std::mutex skinned_mutex_;
  uint32_t defer_mode_ = 1;
  // files/plume-fetchspec (or the cvar): 1 hands the live fetch table to the
  // driver as specialization constants. Part of the pipeline key, so a change
  // builds the pipelines of the shaders it touches afresh.
  uint32_t fetch_spec_mode_ = 1;
  // files/plume-depthstrip (or the cvar): 1 draws the draws that have no
  // fragment shader with the vertex shader's output-less form. Part of the
  // pipeline key.
  uint32_t depth_strip_mode_ = 1;
  // files/plume-patchlevel: percent of the guest's tessellation level the water
  // grid uses (100 = as the game sets it).
  uint32_t patch_level_percent_ = 100;
  // files/plume-shadowmin: shadow casters (depth-only draws into a shadow map)
  // with fewer indices than this are dropped; 0 keeps them all.
  uint32_t shadow_min_indices_ = 0;
  // files/plume-hizskip: 0 tests every draw, 1 (default) samples the meshes that are
  // almost never culled.
  bool hiz_skip_ = true;
  bool rec_enabled_ = true;
  // The frame the command thread is on, for the workers' choice of job.
  std::atomic<uint64_t> pipeline_frame_{0};
};

}  // namespace renut::gpu
