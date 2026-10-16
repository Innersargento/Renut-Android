#pragma once
// GPU backend subsystem Resolve. See backend.h.
#include "types.h"

namespace renut::gpu {

class GpuBackend;

struct Resolve {
  explicit Resolve(GpuBackend& owner) : backend(owner) {}
  GpuBackend& backend;

  // GetResolveInfo for a rectangle that is already known (NativeResolve).
  bool ResolveInfoForRect(int32_t x0, int32_t y0, int32_t x1, int32_t y1, draw_util::ResolveInfo& info);
  std::unique_ptr<RegisterFile> resolve_regs_;
  uint32_t resolve_vertices_ = 0;
  bool ExecuteResolve(const draw_util::ResolveInfo& info, const ResolveSurfaces& surfaces);
  // The picture Photo Mode takes: the game's capture routine (sub_82535F20, hooked in
  // guest/game_fixes.cpp) names the resolve it is about to read with the CPU, and only
  // that one is run, waited for and written into its tiled destination. Everything
  // else the scene resolves stays on the GPU.
  void ReadbackResolve(Resolved& dest, const draw_util::ResolveInfo& info, uint32_t base_address, uint32_t dest_x,
                       uint32_t dest_y, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp, uint32_t scale = 4);
  // Debug: appends a line to <files>/plume-resolve.log while <files>/plume-resolve-log exists.
  void LogResolveLine(const std::string& line);
  uint32_t photo_captures_ = 0;
  bool capture_this_resolve_ = false;
  void NativeResolve(const uint32_t* record);
  // Copies a rectangle of a depth target's depth into an R32_FLOAT texture, or
  // its depth and stencil into an RG32F one.
  bool CopyDepth(Target& source, uint32_t x, uint32_t y, uint32_t w, uint32_t h, RenderTexture* dest,
                 uint32_t dest_x, uint32_t dest_y, bool with_stencil);
  std::unique_ptr<RenderBuffer> depth_staging_;
  // The stencil half of a k_24_8_FLOAT resolve (CopyDepth): the S8 aspect,
  // then depth_stencil_pack.comp joining both into RG32F texels.
  std::unique_ptr<RenderBuffer> stencil_staging_, packed_staging_;
  // Debug (files/plume-validate): the check at the moment of a draw, on the bytes it is about to read: a kept range that
  // differs from guest memory while no write hit is pending is one the GPU would read wrong.
  void ValidateRange(const char* what, uint64_t draw_hash, RenderBuffer* buffer, uint64_t offset, uint32_t guest_address,
                     uint32_t size, xenos::Endian endian);
  uint32_t validate_logged_ = 0;
  uint64_t kept_drop_events_ = 0, validate_ranges_ = 0, validate_stale_ = 0;
  void FlushImagesForNextFrame();
  std::unique_ptr<RenderTexture> snap_;
  RenderTextureLayout snap_layout_ = RenderTextureLayout::UNKNOWN;
  void SnapshotResolved(Resolved& dest);
  std::unordered_map<uint64_t, SkinnedSeen> skinned_seen_;
  void LogSkinned();
  uint64_t resolve_serial_ = 0;
};

}  // namespace renut::gpu
