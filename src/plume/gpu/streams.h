#pragma once
// GPU backend subsystem Streams. See backend.h.
#include "types.h"

namespace renut::gpu {

class GpuBackend;

struct Streams {
  explicit Streams(GpuBackend& owner) : backend(owner) {}
  GpuBackend& backend;

  BufferRange stream_object_{};
  // Upload ring (one frame; reset after every swap).
  uint64_t Allocate(uint64_t size, uint64_t alignment = 16);
  bool UploadGuest(uint32_t address, uint32_t size, xenos::Endian endian, uint64_t& offset);
  // A stream copied into the ring for this draw alone, with the format 17
  // vectors at the given (offset, stride) pairs moved into the layout the
  // shaders decode (RepackFormat17).
  bool UploadRepacked(uint32_t address, uint32_t size, xenos::Endian endian,
                      const std::pair<uint32_t, uint32_t>* elements, uint32_t element_count, uint64_t& offset);
  bool ExportOverlaps(uint32_t address, uint32_t size) const;
  // A guest vertex or index stream, from a buffer of its own that outlives the
  // frame while the guest leaves that memory alone, or copied into the ring.
  //
  // Nearly all of the game's geometry never changes, and copying and swapping
  // every stream into the ring every frame was the command thread's largest
  // single cost after the draw itself (~48 MB a frame in a busy scene). The
  // guest's pages under a kept stream are write-watched (rexglue's physical
  // memory callbacks, the mechanism xenia's shared memory uses): the first
  // write to one drops every stream on it, and a stream dropped often is
  // treated as dynamic and goes back to the ring each frame, unwatched.
  bool GuestStream(uint32_t address, uint32_t size, xenos::Endian endian, RenderBuffer*& buffer,
                   uint64_t& offset);
  void DrainWatchHits();
  static std::pair<uint32_t, uint32_t> WatchHitThunk(void* context, uint32_t start, uint32_t length,
                                                     bool exact_range);
  std::unordered_map<uint64_t, KeptStream> kept_streams_;
  // What the write watch has to look at when the guest writes: the kept streams
  // and the watched textures by 64 KB block of guest memory, so a write finds
  // its few instead of every one (entries of what is gone are skipped, and the
  // index is rebuilt when it has grown far past what lives).
  std::unordered_multimap<uint32_t, uint64_t> kept_block_index_, texture_block_index_;
  static void IndexBlocks(std::unordered_multimap<uint32_t, uint64_t>& index, uint32_t base, uint32_t size,
                          uint64_t key);
  void DropKept(KeptStream& kept);
  // A format-17 stream (see RepackFormat17) kept already repacked, like a kept
  // stream; falls back to UploadRepacked when it cannot be kept.
  bool RepackedStream(uint32_t address, uint32_t size, xenos::Endian endian,
                      const std::pair<uint32_t, uint32_t>* elements, uint32_t element_count,
                      RenderBuffer*& buffer, uint64_t& offset);
  LookupMemo<KeptStream, 13> kept_memo_;
  uint64_t kept_bytes_ = 0;
  uint64_t kept_budget_ = 384ull << 20;
  // bytes of decoded 2D textures; 0 = unlimited
  // Filled by the guest thread that tripped a watch (inside its fault
  // handler), drained by this one.
  std::mutex watch_mutex_;
  std::vector<std::pair<uint32_t, uint32_t>> watch_hits_;
  std::atomic<uint32_t> watch_serial_{0};
  uint32_t watch_seen_ = 0;
  ExportRange* GetExport(uint32_t base, uint32_t size, xenos::Endian endian);
  void RefreshExport(ExportRange& range);
  bool BindStream(uint32_t address, uint32_t size, xenos::Endian endian, RenderBuffer*& buffer,
                  uint64_t& offset);
  void ExportBarrier();
  // What a frame's shaders exported goes back to guest memory once the frame
  // has run, as the hardware would have left it there: the CPU may read it,
  // and a CPU that clears a range again has to be seen doing so - zeros
  // written over the zeros guest memory still held would otherwise go
  // unnoticed and leave the GPU's old contents in place.
  void RecordWritebacks();
  void ApplyWritebacks(uint32_t slot);
  std::array<std::unique_ptr<RenderBuffer>, kPlumeFramesInFlight> writeback_buffers_;
  std::array<uint64_t, kPlumeFramesInFlight> writeback_capacity_{};
  std::array<std::vector<Writeback>, kPlumeFramesInFlight> writebacks_;
  bool CommitConstants(uint8_t* shadow, const void* source, size_t size, uint64_t& offset,
                       bool* written = nullptr);
  const PatchGrid* GetPatchGrid(bool quad, uint32_t level, uint32_t patches);
  std::unordered_map<uint32_t, PatchGrid> patch_grids_;
  std::unordered_map<uint64_t, ShaderWatch> shader_watch_;
  std::unordered_multimap<uint32_t, uint64_t> shader_block_index_;
  // Keyed by guest base. Never overlapping: a new range swallows the ones it touches.
  std::map<uint32_t, ExportRange> exports_;
  std::array<std::vector<std::unique_ptr<RenderBuffer>>, kPlumeFramesInFlight> retired_buffers_;
  uint64_t upload_used_ = 0;
  std::unordered_map<uint64_t, uint64_t> uploaded_streams_;
};

}  // namespace renut::gpu
