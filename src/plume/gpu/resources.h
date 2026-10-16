#pragma once
// Render targets, resolved images, guest textures and the statistics.
#include "formats.h"

namespace renut::gpu {


struct Target {
  std::unique_ptr<RenderTexture> texture;
  RenderFormat format = RenderFormat::UNKNOWN;
  uint32_t width = 0, height = 0;
  bool depth = false;
  RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
  // Serial of the last draw that rendered to it.
  uint64_t last_used = 0;
  // The D3DSurface it is, its RB_COLOR_INFO / RB_DEPTH_INFO, and the frame it
  // was last used in.
  uint32_t surface = 0, surface_info = 0;
  uint64_t used_frame = 0;
  // Quarters of the guest's resolution the host draws it at: 4 is the guest's
  // size, less is a shadow map under the shadow quality option (width and
  // height are host pixels; the guest's viewport, scissor, clears and resolve
  // rectangles are scaled by this).
  uint32_t scale = 4;
  // A depth resolve took this target's image for its texture (AliasDepthResolve):
  // the key of the resolve holding it and the image itself, so a draw that uses
  // the surface again without clearing all of it gets the contents copied back.
  uint64_t restore_key = 0;
  const RenderTexture* restore_image = nullptr;
  // How often this surface was resolved in place and then cleared whole before
  // anything else used it (the pattern of a shadow map), and whether it is
  // between the two now. Two surfaces that alternate into one texture need their
  // old contents copied back only until the pattern has been seen twice;
  // after that the contents are not worth a copy a frame (depth_undefined).
  uint32_t alias_cycles = 0;
  bool aliased_since_clear = false;
  bool depth_undefined = false;
};

// A guest pixel coordinate in a target's host pixels, rounded down or up.
inline uint32_t HostFloor(const Target* target, uint32_t value) {
  return target ? uint32_t(uint64_t(value) * target->scale / 4) : value;
}
inline uint32_t HostCeil(const Target* target, uint32_t value) {
  return target ? uint32_t((uint64_t(value) * target->scale + 3) / 4) : value;
}

// Keys of the render targets: one per surface (see SurfaceTarget).
constexpr uint64_t kSurfaceTargetKey = 1ull << 63;

// A native resolve's source and the depth its clear goes to, by surface, and
// the rectangle in the source's pixels.
struct ResolveSurfaces {
  Target* source = nullptr;
  Target* depth = nullptr;
  uint32_t x = 0, y = 0, width = 0, height = 0;
  // The destination texture object's level 0 (GPU address, 0 if the copy goes
  // elsewhere), its size and pitch, and where the source corner lands in it.
  uint32_t object_base = 0, object_width = 0, object_height = 0, object_pitch = 0;
  int32_t dest_dx = 0, dest_dy = 0;
};

struct Resolved {
  std::unique_ptr<RenderTexture> texture;
  RenderFormat format = RenderFormat::UNKNOWN;
  uint32_t width = 0, height = 0;
  uint32_t descriptor = 0;
  RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
  // Bumped by every resolve into this texture.
  uint64_t generation = 0;
  // Serial of the last resolve into it (see FindResolved).
  uint64_t written = 0;
  // The guest memory under it when last resolved. Resolves are not written
  // back to guest memory, so if these bytes change the CPU has put something
  // else there (a texture streamed into freed render memory) and this image
  // is stale.
  uint64_t guest_hash = 0;
  uint32_t guest_bytes = 0;
  uint32_t guest_pitch = 0, guest_height = 0, guest_bpp = 0;
  uint64_t written_frame = ~0ull;
  uint64_t checked_frame = ~0ull;
  // The texture is sized to the copy's aligned extent (720 rows become 736),
  // but a fetch normalizes coordinates to its own size, so a fetch smaller
  // than the texture samples this copy of its top-left corner instead.
  std::unique_ptr<RenderTexture> fitted;
  uint32_t fitted_width = 0, fitted_height = 0, fitted_descriptor = 0;
  uint64_t fitted_generation = UINT64_MAX;
  RenderTextureLayout fitted_layout = RenderTextureLayout::UNKNOWN;
  // A depth resolve (R32_FLOAT, depth in x) seen through the fetch
  // constant's swizzle, keyed by swizzle | fitted << 12. A Xenos depth
  // texture returns the depth in every channel; the game's depth restore
  // reads it as .w, which an R32_FLOAT view gives as 1.
  struct SwizzledView {
    const RenderTexture* texture = nullptr;
    std::unique_ptr<RenderTextureView> view;
    uint32_t descriptor = 0;
  };
  std::map<uint32_t, SwizzledView> swizzled;
  // Last written by a depth resolve.
  bool depth = false;
  // A draw has fetched this resolve (or it was presented). One that nothing ever samples is a
  // copy the CPU reads, like Photo Mode's picture (see ReadbackResolve).
  bool sampled = false;
  // Quarters of the guest's resolution it was resolved at (Target::scale of
  // the surface it came from): width and height are host pixels.
  uint32_t scale = 4;
  // `texture` is a depth target's own image, not a copy of it: a depth map
  // resolved whole is sampled in place (AliasDepthResolve). `format` is then the
  // depth format, and the views and the fitted copy are made accordingly.
  bool alias = false;
};

struct GuestTexture {
  std::unique_ptr<RenderTexture> texture;
  uint64_t checked_frame = ~0ull;
  uint32_t width = 0, height = 0;
  std::unique_ptr<RenderTextureView> view;  // cube maps
  uint32_t descriptor = 0;
  uint32_t levels = 1, depth = 1;
  uint64_t content = 0;
  bool failed = false;
  RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
  // 2D textures: the guest memory under the base levels and under the mips,
  // write-watched while `watched` (see GetGuestTexture).
  std::array<uint32_t, 2> watch_begin{}, watch_end{};
  bool watched = false;
  // 2D textures: the block format the host image holds (kNone: RGBA8 decoded on
  // the CPU), and whether the compressed upload was tried and did not fit.
  BcKind bc = BcKind::kNone;
  bool bc_blocked = false;
  // Guest mips below the host image's top level (max dimension and quality).
  uint32_t skip = 0;
};

// Bytes a 2D guest texture takes on the host, its mip chain included.
inline uint64_t HostTextureBytes(const GuestTexture& t) {
  const uint64_t base = t.bc != BcKind::kNone
                            ? uint64_t((t.width + 3) / 4) * ((t.height + 3) / 4) * BcBlockBytes(t.bc)
                            : uint64_t(t.width) * t.height * 4;
  return t.levels > 1 ? base * 4 / 3 : base;
}

uint64_t ContentHash(const uint8_t* data, uint32_t span);

// Bytes per element of a memory export format, 0 for the ones renutMemExport
// does not store (narrower than a dword).
uint32_t ExportElementBytes(uint32_t format);

struct Stats {
  uint64_t textures_decoded = 0, texture_failures = 0, textures_bc = 0;
  uint64_t draws = 0, drawn = 0, no_vs = 0, no_ps = 0, bad_shader = 0, skipped_prim = 0,
           no_target = 0, resolves = 0, clears = 0, pipelines = 0, overflow = 0;
  // Render passes begun. A tiler resolves and reloads the whole target at each
  // one, so this says how much of the frame is spent moving tile memory.
  uint64_t passes = 0;
  // Render passes ended early by a layout change, by destination layout.
  std::array<uint64_t, 8> split_by{};
  // vkCmdPipelineBarrier calls for texture layouts, each a wait for idle on Turnip.
  uint64_t barriers = 0;
  // Per-draw GPU work: pipeline changes, vertices submitted, and the draws
  // that take a geometry shader (rectangle lists) or are CPU-tessellated
  // patches (the water) and the vertices those patches expand to.
  uint64_t pipeline_binds = 0, vertices = 0, gs_draws = 0, patch_draws = 0, patch_vertices = 0;
  uint64_t adaptive_patch_draws = 0;  // patch draws laid out on the GPU (vertices not counted)
  // Wall clock, in microseconds: whole frames, the pipelines built inside them
  // and the submit that ends them. What is left is the rest of the draw path.
  uint64_t frame_us = 0, pipeline_us = 0, submit_us = 0;
  // Constant blocks a draw found already in the ring, out of three per draw.
  uint64_t constants_reused = 0;
  // Memory export: draws that export, ranges created, streams read from an
  // exported range whole or pieced together over guest memory, ranges the
  // CPU wrote into again, and the barriers ordering all of that.
  uint64_t export_draws = 0, export_ranges = 0, export_reads = 0, export_merges = 0,
           export_refreshes = 0, export_barriers = 0;
  // Whether the frame is bound by this thread or by the game: the CPU time the
  // GPU backend's thread actually ran (not wall clock), the part of it
  // spent waiting for the GPU to hand a frame slot back, and the bytes it
  // copied into the ring - guest vertex and index streams, constant blocks.
  uint64_t cp_cpu_us = 0, slot_wait_us = 0, stream_bytes = 0, constant_bytes = 0;
  // Streams served from a kept buffer, bytes copied into new kept buffers, and
  // kept streams the guest wrote over.
  uint64_t kept_hits = 0, kept_bytes_made = 0, kept_drops = 0, kept_unwatchable = 0;
  uint64_t band_skipped = 0, band_moved = 0, band_widened = 0;
  // Draws that waited in a window, the windows, and the pipeline changes
  // those windows had in game order against the order they were recorded in.
  uint64_t deferred_draws = 0, defer_windows = 0, defer_binds_before = 0, defer_binds_after = 0;
  uint64_t shadow_culled = 0;  // plume-shadowmin
  // Depth resolves that handed their image over instead of copying it, the
  // ones whose surface was used again and got the contents copied back, and
  // the ones a full clear made unnecessary.
  uint64_t depth_aliased = 0, depth_restored = 0, depth_dropped = 0, depth_abandoned = 0;
  uint64_t twin_skipped = 0;   // MSAA off: per-band copies of a draw already drawn
  // New pipelines by how they were made: found in the driver's cache at the
  // draw, found by a worker, or compiled (and of those, how many waited for a
  // sibling's compile of the same program); the lookups at the draw that
  // missed; and the time spent on lookups at the draw and on compiles.
  uint64_t pipelines_found_here = 0, pipelines_found_ahead = 0, pipelines_compiled = 0, pipelines_waited = 0,
           lookups_missed = 0, lookup_us = 0, compile_us = 0;
  // New pipelines the precompilation had built before their draw came.
  uint64_t pipelines_prebuilt = 0;
  // Hi-Z (hiz_model.h): colour/depth-only draws looked at, the ones with a
  // model and a box, the ones that could not be dropped for lack of either,
  // and the ones whose box lay wholly outside the frustum or the scissor.
  uint64_t hiz_seen[2] = {}, hiz_eligible[2] = {}, hiz_no_model[2] = {}, hiz_no_box[2] = {};
  uint64_t hiz_frustum[2] = {}, hiz_scissor[2] = {}, hiz_skipped[2] = {};
  uint64_t hiz_boxes_made = 0, hiz_box_us = 0, hiz_box_hits = 0, hiz_models = 0, hiz_models_valid = 0,
           hiz_model_rejected = 0, hiz_model_us = 0;
  uint64_t hiz_culled_total = 0;
  // Windows closed, and the draws they held, by reason: 0 elsewhere, 1 state change, 2 full, 3 an immediate
  // draw, 4 something the draw itself needed (a texture, a barrier).
  uint64_t flush_windows[8] = {}, flush_draws[8] = {};
  uint64_t hiz_boxed_draws = 0, hiz_why_not[4] = {}, hiz_small_windows = 0, hiz_small_draws = 0, hiz_windows_all = 0;
  uint64_t hiz_cull_ns = 0;
  uint64_t stream_memo_hits = 0;
  uint64_t hiz_stage_ns[4] = {};  // model lookup, box lookup, constants evaluation, plane/scissor tests (sampled, scaled)
  // Draws with no model by why (renut::hiz::Why), depth-only [0] and colour [1].
  uint64_t hiz_why_model[2][12] = {};
  uint64_t hiz_verified = 0, hiz_violations = 0, hiz_missed_hidden = 0, hiz_missed_empty = 0;
  uint64_t hiz_windows_tested = 0, hiz_records = 0, hiz_held = 0, hiz_test_failed = 0, hiz_answered = 0,
           hiz_occluded = 0, hiz_held_skipped = 0, hiz_held_visible = 0, hiz_predicated_draws = 0;
  // Why a draw was not considered: no indices, primitive, memory export, clip/viewport state, constant base.
  uint64_t hiz_why[5] = {};
};

}  // namespace renut::gpu
