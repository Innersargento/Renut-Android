// Upload ring, kept vertex streams, memory exports and the constant blocks.
#include "internal.h"

namespace renut::gpu {

uint64_t PlumeCommandProcessor::Allocate(uint64_t size, uint64_t alignment) {
  const uint64_t limit = slice_base_ + upload_slice_size_;
  uint64_t offset = (upload_used_ + alignment - 1) & ~(alignment - 1);
  if (offset + size > limit) {
    ++stats_.overflow;
    Flush();
    offset = (upload_used_ + alignment - 1) & ~(alignment - 1);
    if (offset + size > limit) return UINT64_MAX;
  }
  upload_used_ = offset + size;
  return offset;
}

bool PlumeCommandProcessor::UploadGuest(uint32_t address, uint32_t size, xenos::Endian endian,
                                        uint64_t& offset) {
  if (!address || !size || size > (32u << 20)) return false;
  uint64_t key = (uint64_t(address) << 32) ^ (uint64_t(size) << 2) ^ uint64_t(endian);
  auto found = uploaded_streams_.find(key);
  if (found != uploaded_streams_.end() && !(skip_mask_ & 256)) {
    offset = found->second;
    return true;
  }
  offset = Allocate(size, 256);
  if (offset == UINT64_MAX) return false;
  CopySwapped(Pointer(offset), memory_->TranslatePhysical<const uint8_t*>(address), size, endian);
  stats_.stream_bytes += size;
  uploaded_streams_.emplace(key, offset);
  return true;
}

bool PlumeCommandProcessor::UploadRepacked(uint32_t address, uint32_t size, xenos::Endian endian,
                                           const std::pair<uint32_t, uint32_t>* elements,
                                           uint32_t element_count, uint64_t& offset) {
  if (!address || !size || size > (4u << 20)) return false;
  offset = Allocate(size, 256);
  if (offset == UINT64_MAX) return false;
  uint8_t* data = Pointer(offset);
  CopySwapped(data, memory_->TranslatePhysical<const uint8_t*>(address), size, endian);
  for (uint32_t i = 0; i < element_count; ++i) {
    const uint32_t stride = elements[i].second;
    if (!stride) continue;
    for (uint64_t at = elements[i].first; at + 4 <= size; at += stride) {
      uint32_t word;
      std::memcpy(&word, data + at, 4);
      word = RepackFormat17(word);
      std::memcpy(data + at, &word, 4);
    }
  }
  stats_.stream_bytes += size;
  return true;
}

// Whether a memory export range touches the stream: what the GPU wrote there
// is not in guest memory.
bool PlumeCommandProcessor::ExportOverlaps(uint32_t address, uint32_t size) const {
  if (exports_.empty()) return false;
  auto first = exports_.upper_bound(address);
  if (first != exports_.begin()) --first;
  for (auto it = first; it != exports_.end() && it->first < uint64_t(address) + size; ++it) {
    const ExportRange& range = it->second;
    if (range.base < uint64_t(address) + size && address < uint64_t(range.base) + range.size) return true;
  }
  return false;
}

// The guest wrote into watched pages: which pages is all that is known, so
// every kept stream on them is dropped. Called with the guest's global lock
// held, from the thread that wrote, so it only records the range. Returning
// the range itself keeps rexglue from unwatching up to 4 MB around it, which
// would leave other kept streams there unwatched without a call.
std::pair<uint32_t, uint32_t> PlumeCommandProcessor::WatchHitThunk(void* context, uint32_t start,
                                                                   uint32_t length, bool) {
  auto* self = static_cast<PlumeCommandProcessor*>(context);
  {
    std::lock_guard<std::mutex> lock(self->watch_mutex_);
    self->watch_hits_.emplace_back(start, length);
  }
  self->watch_serial_.fetch_add(1, std::memory_order_release);
  return {start, length};
}

// A kept stream the guest wrote over: the buffer goes once the frames in flight
// are done with it, and it counts as dropped (see GuestStream).
void PlumeCommandProcessor::DropKept(KeptStream& kept) {
  ++stream_epoch_;
  // Earlier draws of this frame, and the frame before, may still read it.
  retired_buffers_[slot_].push_back(std::move(kept.buffer));
  kept_bytes_ -= kept.size;
  kept.valid = false;
  ++kept.drops;
  kept.dropped_frame = swaps_;
  ++stats_.kept_drops;
  ++kept_drop_events_;
}

void PlumeCommandProcessor::DrainWatchHits() {
  const uint32_t serial = watch_serial_.load(std::memory_order_acquire);
  if (serial == watch_seen_) return;
  watch_seen_ = serial;
  std::vector<std::pair<uint32_t, uint32_t>> hits;
  {
    std::lock_guard<std::mutex> lock(watch_mutex_);
    hits.swap(watch_hits_);
  }
  for (const auto& [start, length] : hits) {
    if (!length) continue;
    const uint32_t first = start >> 16, last = uint32_t((uint64_t(start) + length - 1) >> 16);
    auto touches = [&](uint32_t base, uint32_t end) { return base < start + uint64_t(length) && start < end; };
    auto drop_texture = [&](GuestTexture& texture) {
      if (texture.watched && (touches(texture.watch_begin[0], texture.watch_end[0]) ||
                              touches(texture.watch_begin[1], texture.watch_end[1]))) {
        texture.watched = false;
      }
    };
    auto drop_kept = [&](KeptStream& kept) {
      if (kept.valid && touches(kept.base, kept.base + kept.size)) DropKept(kept);
    };
    auto drop_shader = [&](ShaderWatch& entry) {
      if (entry.valid && touches(entry.base, entry.base + entry.bytes)) {
        entry.valid = false;
        ++entry.drops;
        entry.dropped_frame = swaps_;
        ++shader_cache_drops_;
      }
    };
    if (last - first > 1024) {
      // A huge range: every one is looked at.
      for (auto& [key, texture] : guest_textures_) drop_texture(texture);
      for (auto& [key, kept] : kept_streams_) drop_kept(kept);
      for (auto& [key, entry] : shader_watch_) drop_shader(entry);
      continue;
    }
    for (uint32_t block = first; block <= last; ++block) {
      auto textures = texture_block_index_.equal_range(block);
      for (auto it = textures.first; it != textures.second; ++it) {
        auto found = guest_textures_.find(it->second);
        if (found != guest_textures_.end()) drop_texture(found->second);
      }
      auto kepts = kept_block_index_.equal_range(block);
      for (auto it = kepts.first; it != kepts.second; ++it) {
        auto found = kept_streams_.find(it->second);
        if (found != kept_streams_.end()) drop_kept(found->second);
      }
      auto shaders = shader_block_index_.equal_range(block);
      for (auto it = shaders.first; it != shaders.second; ++it) {
        auto found = shader_watch_.find(it->second);
        if (found != shader_watch_.end()) drop_shader(found->second);
      }
    }
  }
  if (shader_block_index_.size() > 200000) {
    shader_block_index_.clear();
    for (auto& [key, entry] : shader_watch_) {
      if (entry.valid) IndexBlocks(shader_block_index_, entry.base, entry.bytes, key);
    }
  }
  // Entries of what is gone pile up: rebuilt from what is live.
  if (kept_block_index_.size() > 400000) {
    kept_block_index_.clear();
    for (auto& [key, kept] : kept_streams_) {
      if (kept.valid) IndexBlocks(kept_block_index_, kept.base, kept.size, key);
    }
  }
  if (texture_block_index_.size() > 400000) {
    texture_block_index_.clear();
    for (auto& [key, texture] : guest_textures_) {
      if (!texture.watched) continue;
      for (uint32_t which = 0; which < 2; ++which) {
        if (texture.watch_end[which] > texture.watch_begin[which]) {
          IndexBlocks(texture_block_index_, texture.watch_begin[which],
                      texture.watch_end[which] - texture.watch_begin[which], key);
        }
      }
    }
  }
  // Boxes the Hi-Z culling made from memory the guest has written.
  if (!hiz_boxes_.empty()) {
    for (const auto& [start, length] : hits) HizDropBoxes(start, length);
  }
}

bool PlumeCommandProcessor::RepackedStream(uint32_t address, uint32_t size, xenos::Endian endian,
                                           const std::pair<uint32_t, uint32_t>* elements, uint32_t element_count,
                                           RenderBuffer*& buffer, uint64_t& offset) {
  buffer = upload_.get();
  if (!keep_streams_ || !address || !size || size > (4u << 20)) {
    return UploadRepacked(address, size, endian, elements, element_count, offset);
  }
  DrainWatchHits();
  uint64_t key = 0xC2B2AE3D27D4EB4Full ^ (uint64_t(address) << 20) ^ (uint64_t(size) << 3) ^ uint64_t(endian);
  for (uint32_t i = 0; i < element_count; ++i) {
    key = (key ^ ((uint64_t(elements[i].first) << 32) | elements[i].second)) * 0x100000001B3ull;
  }
  KeptStream& kept = kept_streams_[key];
  kept.last_used = swaps_;
  if (kept.valid && kept.base == address && kept.size == size) {
    buffer = kept.buffer.get();
    offset = 0;
    ++stats_.kept_hits;
    return true;
  }
  const bool dynamic = kept.drops >= 3 && swaps_ - kept.dropped_frame < 900;
  if (dynamic || kept_bytes_ + size > kept_budget_ ||
      memory_->GetPhysicalHeap()->QueryRangeAccess(address, address + size - 1) !=
          rex::memory::PageAccess::kReadWrite) {
    return UploadRepacked(address, size, endian, elements, element_count, offset);
  }
  auto made = device_->createBuffer(RenderBufferDesc::UploadBuffer(
      size, RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX | RenderBufferFlag::DEVICE_ADDRESSABLE |
                RenderBufferFlag::STORAGE));
  if (!made) return UploadRepacked(address, size, endian, elements, element_count, offset);
  // Watch first, copy second: a write that lands during the copy still drops it.
  memory_->EnablePhysicalMemoryAccessCallbacks(address, size, true, false);
  auto* data = static_cast<uint8_t*>(made->map());
  CopySwapped(data, memory_->TranslatePhysical<const uint8_t*>(address), size, endian);
  for (uint32_t i = 0; i < element_count; ++i) {
    const uint32_t stride = elements[i].second;
    if (!stride) continue;
    for (uint64_t at = elements[i].first; at + 4 <= size; at += stride) {
      uint32_t word;
      std::memcpy(&word, data + at, 4);
      word = RepackFormat17(word);
      std::memcpy(data + at, &word, 4);
    }
  }
  made->unmap();
  kept.buffer = std::move(made);
  kept.base = address;
  kept.size = size;
  kept.endian = uint32_t(endian);
  kept.made_frame = swaps_;
  kept.valid = true;
  IndexBlocks(kept_block_index_, address, size, key);
  kept_bytes_ += size;
  stats_.kept_bytes_made += size;
  stats_.stream_bytes += size;
  buffer = kept.buffer.get();
  offset = 0;
  return true;
}

bool PlumeCommandProcessor::GuestStream(uint32_t address, uint32_t size, xenos::Endian endian,
                                        RenderBuffer*& buffer, uint64_t& offset) {
  // Streams are kept up to this much in all; past it they are copied as before.
  const uint64_t kKeptBudget = kept_budget_;
  buffer = upload_.get();
  if (!keep_streams_ || !address || !size || size > (32u << 20)) {
    return UploadGuest(address, size, endian, offset);
  }
  DrainWatchHits();
  // Kept by aligned window rather than by the exact stream: every draw of a
  // mesh names its own stretch of the same index buffer, and one entry per
  // stretch made tens of thousands of kept buffers, each looked up and each
  // checked against every write.
  // Native lists: a stream inside the vertex or index buffer it comes from is
  // kept as that whole buffer, which is exactly what the guest allocated -
  // no window running into pages beyond it.
  constexpr uint32_t kWindow = 16u << 10;
  uint32_t window_base = address & ~(kWindow - 1);
  uint64_t window_end =
      std::min<uint64_t>((uint64_t(address) + size + kWindow - 1) & ~uint64_t(kWindow - 1), 0x20000000ull);
  if (stream_object_.size && address >= stream_object_.base &&
      uint64_t(address) + size <= uint64_t(stream_object_.base) + stream_object_.size &&
      uint64_t(stream_object_.base) + stream_object_.size <= 0x20000000ull) {
    window_base = stream_object_.base;
    window_end = uint64_t(stream_object_.base) + stream_object_.size;
    ++object_streams_;
  } else {
    ++window_streams_;
  }
  if (window_end <= address + uint64_t(size) - 1 || window_end - window_base > (32u << 20)) {
    return UploadGuest(address, size, endian, offset);
  }
  const uint32_t window_size = uint32_t(window_end - window_base);
  const uint64_t key = (uint64_t(window_base) << 32) ^ (uint64_t(window_size) << 2) ^ uint64_t(endian);
  KeptStream* kept_node = kept_memo_.Find(key);
  if (!kept_node) {
    kept_node = &kept_streams_[key];
    kept_memo_.Put(key, kept_node);
  }
  KeptStream& kept = *kept_node;
  kept.last_used = swaps_;
  if (kept.valid) {
    buffer = kept.buffer.get();
    offset = address - window_base;
    ++stats_.kept_hits;
    return true;
  }
  // Written over three times: the guest rewrites it, and a watch would only
  // cost its thread a fault a frame. Tried again after a while.
  const bool dynamic = kept.drops >= 3 && swaps_ - kept.dropped_frame < 900;
  if (dynamic || kept_bytes_ + window_size > kKeptBudget) return UploadGuest(address, size, endian, offset);
  // The window runs past the stream to 16 KB boundaries, and a page there the
  // guest has not committed (or keeps read-only) gets no watch - rexglue only
  // watches writable pages, and committing one later tells nobody. A mesh the
  // game then streams into it was drawn from the stale copy: screen-sized
  // polygons with garbage textures, minutes into a level, until the window
  // happened to be dropped. Such a window goes through the ring, and is looked
  // at again once it would have stopped counting as dynamic.
  if (memory_->GetPhysicalHeap()->QueryRangeAccess(window_base, window_base + window_size - 1) !=
      rex::memory::PageAccess::kReadWrite) {
    kept.drops = std::max<uint32_t>(kept.drops, 3);
    kept.dropped_frame = swaps_;
    ++stats_.kept_unwatchable;
    return UploadGuest(address, size, endian, offset);
  }
  auto made = device_->createBuffer(RenderBufferDesc::UploadBuffer(
      window_size, RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX | RenderBufferFlag::DEVICE_ADDRESSABLE |
                RenderBufferFlag::STORAGE));
  if (!made) return UploadGuest(address, size, endian, offset);
  // Watch first, copy second: a write that lands during the copy still drops it.
  memory_->EnablePhysicalMemoryAccessCallbacks(window_base, window_size, true, false);
  auto* data = static_cast<uint8_t*>(made->map());
  CopySwapped(data, memory_->TranslatePhysical<const uint8_t*>(window_base), window_size, endian);
  made->unmap();
  kept.buffer = std::move(made);
  kept.base = window_base;
  kept.size = window_size;
  kept.endian = uint32_t(endian);
  kept.made_frame = swaps_;
  kept.valid = true;
  IndexBlocks(kept_block_index_, window_base, window_size, key);
  kept_bytes_ += window_size;
  stats_.kept_bytes_made += window_size;
  buffer = kept.buffer.get();
  offset = address - window_base;
  return true;
}

// Orders memory export against everything around it: shader stores and the
// copies into exported ranges before the vertex fetches, stores and copies
// that follow, and earlier fetches before later stores. It has to end the
// render pass; the pass reopens on the same framebuffer at the next draw.
void PlumeCommandProcessor::ExportBarrier() {
  auto* vulkan = static_cast<VulkanCommandList*>(L());
  vulkan->endActiveRenderPass();
  VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                          VK_ACCESS_TRANSFER_WRITE_BIT;
  const VkPipelineStageFlags stages = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                      VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                      VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT |
                                      VK_PIPELINE_STAGE_TRANSFER_BIT;
  vkCmdPipelineBarrier(vulkan->vk, stages, stages, 0, 1, &barrier, 0, nullptr, 0, nullptr);
  ++stats_.export_barriers;
  for (auto& [base, range] : exports_) range.written = range.read = false;
}

// The range a vertex shader exports into, holding [base, base + size). A new
// one starts as a copy of guest memory and takes over, with their contents,
// every range it overlaps.
PlumeCommandProcessor::ExportRange* PlumeCommandProcessor::GetExport(uint32_t base, uint32_t size,
                                                                     xenos::Endian endian) {
  uint32_t lo = base, hi = base + size;
  for (auto& [key, range] : exports_) {
    if (range.base <= lo && hi <= range.base + range.size) return &range;
  }
  std::vector<uint32_t> merged;
  for (bool grew = true; grew;) {
    grew = false;
    for (auto& [key, range] : exports_) {
      if (range.base < hi && lo < range.base + range.size &&
          std::find(merged.begin(), merged.end(), key) == merged.end()) {
        merged.push_back(key);
        lo = std::min(lo, range.base);
        hi = std::max(hi, range.base + range.size);
        grew = true;
      }
    }
  }
  ExportRange fresh;
  fresh.base = lo;
  fresh.size = hi - lo;
  // Held the way every vertex stream is, k8in32, whatever the endianness of
  // the exports into it: renutMemExport converts each store to that form.
  fresh.endian = xenos::Endian::k8in32;
  fresh.buffer = device_->createBuffer(RenderBufferDesc::DefaultBuffer(
      fresh.size, RenderBufferFlag::VERTEX | RenderBufferFlag::STORAGE | RenderBufferFlag::CONSTANT |
                      RenderBufferFlag::DEVICE_ADDRESSABLE));
  if (!fresh.buffer) return nullptr;
  fresh.address = fresh.buffer->getDeviceAddress();
  const uint64_t offset = Allocate(fresh.size, 256);
  if (offset == UINT64_MAX) return nullptr;
  const uint8_t* memory = memory_->TranslatePhysical<const uint8_t*>(lo);
  fresh.snapshot.assign(memory, memory + fresh.size);
  CopySwapped(Pointer(offset), memory, fresh.size, fresh.endian);
  bool pending = false;
  for (uint32_t key : merged) pending |= exports_[key].written;
  if (pending) ExportBarrier();
  L()->copyBufferRegion(RenderBufferReference(fresh.buffer.get(), 0),
                          RenderBufferReference(upload_.get(), offset), fresh.size);
  if (!merged.empty()) {
    // The guest's bytes first, then what the GPU wrote over them.
    ExportBarrier();
    for (uint32_t key : merged) {
      ExportRange& old = exports_[key];
      L()->copyBufferRegion(RenderBufferReference(fresh.buffer.get(), old.base - lo),
                              RenderBufferReference(old.buffer.get(), 0), old.size);
      retired_buffers_[slot_].push_back(std::move(old.buffer));
      ++export_epoch_;
      exports_.erase(key);
    }
  }
  fresh.written = true;
  fresh.checked_frame = swaps_;
  fresh.exported_frame = swaps_;
  ++stats_.export_ranges;
  static uint32_t logged = 0;
  if (logged < 64) {
    ++logged;
    REXLOG_INFO("Renut: Plume export range {:08X}+{:X} endian {} (asked {:08X}+{:X}, merged {})", lo,
                fresh.size, uint32_t(endian), base, size, merged.size());
  }
  ++export_epoch_;
  return &exports_.emplace(lo, std::move(fresh)).first->second;
}

void PlumeCommandProcessor::RecordWritebacks() {
  auto& list = writebacks_[slot_];
  list.clear();
  if (!writeback_) return;
  uint64_t total = 0;
  for (const auto& [key, range] : exports_) {
    if (range.exported_frame == swaps_) total += range.size;
  }
  if (!total) return;
  if (total > writeback_capacity_[slot_]) {
    // The slot's last frame has finished (BeginFrame waited for it), so the
    // old buffer is free to go.
    const uint64_t capacity = std::max<uint64_t>(total + total / 2, 1u << 20);
    writeback_buffers_[slot_] = device_->createBuffer(RenderBufferDesc::ReadbackBuffer(capacity));
    writeback_capacity_[slot_] = writeback_buffers_[slot_] ? capacity : 0;
    if (!writeback_buffers_[slot_]) return;
  }
  ExportBarrier();
  uint64_t at = 0;
  for (const auto& [key, range] : exports_) {
    if (range.exported_frame != swaps_) continue;
    L()->copyBufferRegion(RenderBufferReference(writeback_buffers_[slot_].get(), at),
                            RenderBufferReference(range.buffer.get(), 0), range.size);
    list.push_back({range.base, range.size, at, range.buffer.get()});
    at += range.size;
  }
}

void PlumeCommandProcessor::ApplyWritebacks(uint32_t slot) {
  auto& list = writebacks_[slot];
  if (list.empty()) return;
  if (!writeback_) {
    list.clear();
    return;
  }
  const auto* data = static_cast<const uint8_t*>(writeback_buffers_[slot]->map());
  constexpr uint32_t kBlock = 256;
  alignas(16) uint8_t block[kBlock];
  for (const Writeback& w : list) {
    auto found = exports_.find(w.base);
    if (found == exports_.end() || found->second.buffer.get() != w.buffer ||
        found->second.size != w.size) {
      continue;
    }
    ExportRange& range = found->second;
    uint8_t* guest = memory_->TranslatePhysical<uint8_t*>(range.base);
    for (uint32_t at = 0; at < range.size; at += kBlock) {
      const uint32_t length = std::min(kBlock, range.size - at);
      // A block the CPU wrote since is left alone: its bytes win, and the
      // next RefreshExport takes them in.
      if (std::memcmp(guest + at, range.snapshot.data() + at, length) != 0) continue;
      CopySwapped(block, data + w.offset + at, length, range.endian);
      if (std::memcmp(block, range.snapshot.data() + at, length) == 0) continue;
      std::memcpy(guest + at, block, length);
      std::memcpy(range.snapshot.data() + at, block, length);
    }
  }
  writeback_buffers_[slot]->unmap();
  list.clear();
}

// Brings in what the CPU wrote to an exported range since it was last looked
// at, once a frame.
void PlumeCommandProcessor::RefreshExport(ExportRange& range) {
  if (range.checked_frame == swaps_) return;
  range.checked_frame = swaps_;
  const uint8_t* memory = memory_->TranslatePhysical<const uint8_t*>(range.base);
  if (std::memcmp(memory, range.snapshot.data(), range.size) == 0) return;
  if (range.written) ExportBarrier();
  constexpr uint32_t kBlock = 256;
  auto differs = [&](uint32_t at) {
    return std::memcmp(memory + at, range.snapshot.data() + at, std::min(kBlock, range.size - at)) != 0;
  };
  for (uint32_t at = 0; at < range.size;) {
    if (!differs(at)) {
      at += kBlock;
      continue;
    }
    uint32_t end = at;
    while (end < range.size && differs(end)) end += kBlock;
    end = std::min(end, range.size);
    const uint64_t offset = Allocate(end - at, 256);
    if (offset == UINT64_MAX) break;
    CopySwapped(Pointer(offset), memory + at, end - at, range.endian);
    L()->copyBufferRegion(RenderBufferReference(range.buffer.get(), at),
                            RenderBufferReference(upload_.get(), offset), end - at);
    std::memcpy(range.snapshot.data() + at, memory + at, end - at);
    at = end;
  }
  range.written = true;
  ++stats_.export_refreshes;
}

// Where a draw reads a vertex stream: the exported range that holds it whole,
// or the upload ring - with the exported stretches of a stream that is only
// partly exported copied over the guest's bytes.
bool PlumeCommandProcessor::BindStream(uint32_t address, uint32_t size, xenos::Endian endian,
                                       RenderBuffer*& buffer, uint64_t& offset) {
  buffer = upload_.get();
  if (exports_.empty()) return GuestStream(address, size, endian, buffer, offset);
  std::vector<ExportRange*> overlapping;
  // The ranges are disjoint (GetExport merges whatever a new one overlaps) and
  // keyed by their base, so only the one starting at or before the stream and
  // those starting inside it can touch it. Walking the whole map for every
  // stream of every draw was ~4% of this thread.
  auto first = exports_.upper_bound(address);
  if (first != exports_.begin()) --first;
  for (auto it = first; it != exports_.end() && it->first < uint64_t(address) + size; ++it) {
    ExportRange& range = it->second;
    if (address >= range.base && uint64_t(address) + size <= uint64_t(range.base) + range.size) {
      RefreshExport(range);
      if (range.written) ExportBarrier();
      range.read = true;
      buffer = range.buffer.get();
      offset = address - range.base;
      ++stats_.export_reads;
      Trace("  stream {:08X}+{:X} from export {:08X}+{:X}", address, size, range.base, range.size);
      return true;
    }
    if (range.base < address + size && address < range.base + range.size) overlapping.push_back(&range);
  }
  if (overlapping.empty()) return GuestStream(address, size, endian, buffer, offset);
  // A stream that is mostly exported is typically the pool the particle
  // emitters export into piece by piece, drawn whole. Growing a range over it
  // lets every later draw read it straight away instead of piecing it
  // together again, two barriers each time. One that is mostly guest data is
  // pieced together instead: a range only catches up with the CPU's writes
  // once a frame, and the rest of such a buffer may be written between draws.
  uint32_t exported = 0;
  for (ExportRange* range : overlapping) {
    exported += std::min(address + size, range->base + range->size) - std::max(address, range->base);
  }
  if (size <= (4u << 20) && exported * 2 >= size) {
    if (ExportRange* range = GetExport(address, size, endian)) {
      RefreshExport(*range);
      ExportBarrier();
      range->read = true;
      buffer = range->buffer.get();
      offset = address - range->base;
      ++stats_.export_reads;
      return true;
    }
  }
  if (!address || !size || size > (32u << 20)) return false;
  offset = Allocate(size, 256);
  if (offset == UINT64_MAX) return false;
  CopySwapped(Pointer(offset), memory_->TranslatePhysical<const uint8_t*>(address), size, endian);
  bool pending = false;
  for (ExportRange* range : overlapping) {
    RefreshExport(*range);
    pending |= range->written;
  }
  if (pending) ExportBarrier();
  for (ExportRange* range : overlapping) {
    const uint32_t lo = std::max(address, range->base);
    const uint32_t hi = std::min(address + size, range->base + range->size);
    L()->copyBufferRegion(RenderBufferReference(upload_.get(), offset + (lo - address)),
                            RenderBufferReference(range->buffer.get(), lo - range->base), hi - lo);
  }
  ExportBarrier();
  for (ExportRange* range : overlapping) range->read = true;
  ++stats_.export_merges;
  return true;
}

// Puts `source` in the upload ring unless an earlier draw already left the
// same bytes there - see the shadow buffers.
bool PlumeCommandProcessor::CommitConstants(uint8_t* shadow, const void* source, size_t size,
                                            uint64_t& offset, bool* written) {
  // Nothing written into the block since it was last placed: the same bytes,
  // without comparing them.
  if (offset != UINT64_MAX && written && !*written && !(skip_mask_ & 4194304)) {
    ++stats_.constants_reused;
    return true;
  }
  if (written) *written = false;
  if (offset != UINT64_MAX && !(skip_mask_ & 4194304) && std::memcmp(shadow, source, size) == 0) {
    ++stats_.constants_reused;
    return true;
  }
  // Allocate can flush the ring, which drops every offset including this one.
  const uint64_t at = Allocate(size, 256);
  if (at == UINT64_MAX) return false;
  std::memcpy(shadow, source, size);
  std::memcpy(Pointer(at), source, size);
  stats_.constant_bytes += size;
  offset = at;
  return true;
}

// A triangle patch's grid has level + 1 - row points in row `row`, weighted by
// their distance along each edge; a quad patch's is (level + 1)^2 points. The
// triangles are the ones, in the order, the unindexed expansion drew.
const PlumeCommandProcessor::PatchGrid* PlumeCommandProcessor::GetPatchGrid(bool quad, uint32_t level,
                                                                            uint32_t patches) {
  PatchGrid& grid = patch_grids_[level | (quad ? 0x100u : 0u)];
  if (grid.patches >= patches) return &grid;
  const uint32_t capacity = std::max(patches, grid.patches * 2);
  const uint32_t per_patch = quad ? (level + 1) * (level + 1) : (level + 1) * (level + 2) / 2;
  const uint32_t indices_per_patch = level * level * (quad ? 6 : 3);
  auto points = device_->createBuffer(RenderBufferDesc::UploadBuffer(
      uint64_t(capacity) * per_patch * 4 * sizeof(float), RenderBufferFlag::VERTEX));
  auto indices = device_->createBuffer(RenderBufferDesc::UploadBuffer(
      uint64_t(capacity) * indices_per_patch * sizeof(uint32_t), RenderBufferFlag::INDEX));
  if (!points || !indices) return grid.patches ? &grid : nullptr;
  auto* point = static_cast<float*>(points->map());
  auto* index = static_cast<uint32_t*>(indices->map());
  const float step = 1.0f / float(level);
  // The first point of each row of a triangle patch.
  auto row_start = [&](uint32_t row) { return row * (level + 1) - row * (row - 1) / 2; };
  for (uint32_t patch = 0; patch < capacity; ++patch) {
    const uint32_t first = patch * per_patch;
    if (quad) {
      // A patch-indexed quad domain shader takes the patch index in r0.x and
      // the domain location in r0.yz, and r1.x is zero; the recompiled shader
      // reads r0 as iDomainPatch.zyx and r1.x as its w.
      for (uint32_t v = 0; v <= level; ++v) {
        for (uint32_t u = 0; u <= level; ++u) {
          *point++ = float(v) * step;
          *point++ = float(u) * step;
          *point++ = float(patch);
          *point++ = 0.0f;
        }
      }
      auto at = [&](uint32_t u, uint32_t v) { return first + v * (level + 1) + u; };
      for (uint32_t v = 0; v < level; ++v) {
        for (uint32_t u = 0; u < level; ++u) {
          const uint32_t quad_indices[6] = {at(u, v),     at(u + 1, v),     at(u + 1, v + 1),
                                            at(u, v),     at(u + 1, v + 1), at(u, v + 1)};
          std::memcpy(index, quad_indices, sizeof(quad_indices));
          index += 6;
        }
      }
    } else {
      for (uint32_t row = 0; row <= level; ++row) {
        for (uint32_t col = 0; col + row <= level; ++col) {
          *point++ = float(col) * step;
          *point++ = float(level - row - col) * step;
          *point++ = float(row) * step;
          *point++ = float(patch);
        }
      }
      auto at = [&](uint32_t row, uint32_t col) { return first + row_start(row) + col; };
      for (uint32_t row = 0; row < level; ++row) {
        for (uint32_t col = 0; col + row < level; ++col) {
          *index++ = at(row, col);
          *index++ = at(row, col + 1);
          *index++ = at(row + 1, col);
          if (col + row + 1 < level) {
            *index++ = at(row, col + 1);
            *index++ = at(row + 1, col + 1);
            *index++ = at(row + 1, col);
          }
        }
      }
    }
  }
  points->unmap();
  indices->unmap();
  // Draws recorded earlier this frame may still read the old grid.
  if (grid.points) retired_buffers_[slot_].push_back(std::move(grid.points));
  if (grid.indices) retired_buffers_[slot_].push_back(std::move(grid.indices));
  grid.points = std::move(points);
  grid.indices = std::move(indices);
  grid.patches = capacity;
  grid.points_per_patch = per_patch;
  grid.indices_per_patch = indices_per_patch;
  REXLOG_INFO("Renut: Plume patch grid {} level {} for {} patches ({} points, {} indices each)",
              quad ? "quad" : "triangle", level, capacity, per_patch, indices_per_patch);
  return &grid;
}

}  // namespace renut::gpu
