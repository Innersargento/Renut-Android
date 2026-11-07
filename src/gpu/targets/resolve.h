#pragma once
// GPU backend subsystem Resolve. See backend.h.
#include "gpu/core/types.h"

namespace renut::gpu {

class GpuBackend;

// What a resolve asks of the host, decoded from a record's copy words (Resolve::DecodeResolve).
struct ResolveCopy {
  bool depth = false;  // the source is the depth surface
  bool clear_color = false, clear_depth = false;
  uint32_t dest_format = 0;  // a xenos::TextureFormat; a depth copy takes the depth surface's own
  uint32_t endian = 0;       // the destination's xenos::Endian
  bool swap = false;         // red and blue swapped
  uint32_t color_clear = 0, color_clear_lo = 0, depth_clear = 0;
};

struct Resolve {
  explicit Resolve(GpuBackend& owner) : backend(owner) {}
  GpuBackend& backend;

  // The copy words of a resolve record (kOpResolve) as the host needs them; false when the record
  // asks for something the host does not do.
  bool DecodeResolve(const uint32_t* record, ResolveCopy& out);
  bool ExecuteResolve(const ResolveCopy& copy, const ResolveSurfaces& surfaces);
  // The picture Photo Mode takes (kOpPhoto, from native_d3d::CapturePhoto): render target 0 as it
  // stands after the draws before the record, run, waited for, and written into the photo texture's
  // rows as the CPU reads a linear X8R8G8B8 texture. Everything else the scene resolves stays on the
  // GPU.
  void NativePhoto(const uint32_t* record);
  // Debug: appends a line to <files>/plume-resolve.log while <files>/plume-resolve-log exists.
  void LogResolveLine(const std::string& line);
  // The lines wait here and are written once a frame (Swap::IssueSwap), not one file open each.
  std::string log_buffer_;
  void FlushResolveLog();
  uint32_t photo_captures_ = 0, volume_logged_ = 0;
  void NativeResolve(const uint32_t* record);
  // Copies a rectangle of a depth target's depth into an R32_FLOAT texture, or
  // its depth and stencil into an RG32F one.
  bool CopyDepth(Target& source, uint32_t x, uint32_t y, uint32_t w, uint32_t h, RenderTexture* dest,
                 uint32_t dest_x, uint32_t dest_y, bool with_stencil);
  // The same as CopyDepth for a depth target's depth and stencil into an RG32F texture made with the storage
  // flag, in one compute pass (depth_resolve.comp) that reads the two aspects as sampled images. False, with
  // nothing recorded, when the pipeline or a view could not be made: the caller then uses CopyDepth. On success
  // the source is left in SHADER_READ and `dest_layout` is GENERAL.
  bool ResolveDepthStencil(Target& source, uint32_t x, uint32_t y, uint32_t w, uint32_t h, RenderTexture* dest,
                           RenderTextureLayout& dest_layout, uint32_t dest_x, uint32_t dest_y);
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
