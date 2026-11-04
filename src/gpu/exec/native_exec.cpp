// The native lists the D3D front end records, executed.
#include "gpu/core/internal.h"

namespace renut::gpu {

// A state record: `count` words of the DrawState from word `first` (RENUT_STATE_WORD).
void Lists::NativeWrite(uint32_t first, const uint32_t* values, uint32_t count) {
  if (first >= kStateConstantsFirst) {
    // The constants, the fetch constants, the booleans and the loops: the guest writes thousands of
    // them a frame, compared with what is here so that only a change marks a block.
    StoreConstants(first, values, count);
    return;
  }
  std::memcpy(reinterpret_cast<uint32_t*>(&backend.state_) + first, values, size_t(count) * sizeof(uint32_t));
}

// A draw record.
void Lists::NativeDraw(uint32_t flags, uint32_t initiator, uint32_t dma_base, uint32_t dma_size) {
  DrawState& state = backend.state_;
  state.draw_initiator.value = initiator;
  const bits::VGT_DRAW_INITIATOR draw = state.draw_initiator;
  IndexBufferInfo info;
  bool indexed = false;
  if (draw.source_select == xenos::SourceSelect::kDMA) {
    state.dma_base = dma_base;
    state.dma_size.value = dma_size;
    const bits::VGT_DMA_SIZE size = state.dma_size;
    const uint32_t bytes = draw.index_size == xenos::IndexFormat::kInt16 ? 2 : 4;
    info.guest_base = dma_base & ~(bytes - 1);
    info.endianness = size.swap_mode;
    info.format = draw.index_size;
    info.length = size.num_words * bytes;
    info.count = draw.num_indices;
    indexed = true;
  } else if (draw.source_select != xenos::SourceSelect::kAutoIndex) {
    ++native_bad_;
    return;
  }
  const auto viz = state.viz_query;
  if (viz.viz_query_ena && viz.kill_pix_post_hi_z) return;
  ++native_draws_;
  draw_pass_ = uint8_t((flags >> native_d3d::kDrawPassShift) & 0xF);
  draw_mode_ = uint8_t((flags >> native_d3d::kDrawModeShift) & 7);
  backend.draw.IssueDraw(draw.prim_type, draw.num_indices, indexed ? &info : nullptr,
            xenos::IsMajorModeExplicit(draw.major_mode, draw.prim_type));
  draw_pass_ = 0;
}

bool Lists::RunNative(uint32_t until) {
  bool found = false;
  for (;;) {
    if (native_stack_.empty()) {
      if (native_queue_.empty()) {
        std::vector<std::shared_ptr<const native_d3d::List>> more;
        native_d3d::TakePublished(more);
        for (auto& list : more) native_queue_.push_back(std::move(list));
        if (native_queue_.empty()) break;
      }
      native_stack_.push_back({std::move(native_queue_.front()), 0});
      native_queue_.pop_front();
    }
    NativeFrame& f = native_stack_.back();
    const std::vector<uint32_t>& words = f.list->words;
    if (f.pos >= words.size()) {
      native_stack_.pop_back();
      if (native_stack_.empty()) native_d3d::NoteListExecuted();
      continue;
    }
    const uint32_t* w = words.data() + f.pos;
    const uint32_t op = w[0] & 0xFF;
    switch (op) {
      case native_d3d::kOpState: {
        const uint32_t count = w[0] >> 8, first = w[1];
        if (f.pos + 2 + count > words.size() || first + count > kStateWords) {
          ++native_bad_;
          f.pos = words.size();
          break;
        }
        NativeWrite(first, w + 2, count);
        f.pos += 2 + count;
        break;
      }
      case native_d3d::kOpShader: {
        const uint32_t fetch_count = (w[0] >> 8) & 0xFF;
        const uint32_t program = (w[0] >> native_d3d::kShaderProgramShift) & 3;
        if (f.pos + 5 + size_t(fetch_count) * 4 > words.size()) {
          ++native_bad_;
          f.pos = words.size();
          break;
        }
        const auto type = xenos::ShaderType(w[1] & 3);
        const uint32_t address = w[1] & ~3u, dwords = w[2];
        const auto* ucode = backend.memory_->TranslatePhysical<const uint32_t*>(address);
        const uint64_t hash = uint64_t(w[3]) | uint64_t(w[4]) << 32;
        Shader* shader;
        if (fetch_count && dwords) {
          shader = backend.shaders.LoadShaderPatched(type, hash, program, ucode, dwords, w + 5, fetch_count);
        } else if (hash && dwords) {
          shader = backend.shaders.LoadShaderIdentity(type, hash, program, ucode, dwords);
        } else {
          shader = backend.shaders.LoadShaderByContents(type, ucode, dwords);
        }
        if (type == xenos::ShaderType::kVertex) {
          active_vertex_shader_ = shader;
        } else {
          active_pixel_shader_ = shader;
        }
        f.pos += 5 + size_t(fetch_count) * 4;
        break;
      }
      case native_d3d::kOpDraw:
        f.pos += 6;
        NativeDraw(w[0] >> 8, w[2], w[3], w[4]);
        break;
      case native_d3d::kOpRun: {
        std::shared_ptr<const native_d3d::List> child = w[1] < f.list->runs.size() ? f.list->runs[w[1]] : nullptr;
        f.pos += 2;
        if (child) native_stack_.push_back({std::move(child), 0});
        break;
      }
      case native_d3d::kOpSwap:
        f.pos += 4;
        break;
      case native_d3d::kOpResolve:
        if (f.pos + native_d3d::kResolveWords > words.size()) {
          ++native_bad_;
          f.pos = words.size();
          break;
        }
        f.pos += native_d3d::kResolveWords;
        backend.resolve.NativeResolve(w);
        break;
      case native_d3d::kOpClear:
        if (f.pos + native_d3d::kClearWords > words.size()) {
          ++native_bad_;
          f.pos = words.size();
          break;
        }
        f.pos += native_d3d::kClearWords;
        NativeClear(w);
        break;
      case native_d3d::kOpPhoto:
        if (f.pos + native_d3d::kPhotoWords > words.size()) {
          ++native_bad_;
          f.pos = words.size();
          break;
        }
        f.pos += native_d3d::kPhotoWords;
        backend.resolve.NativePhoto(w);
        break;
      case native_d3d::kOpFence:
        if (f.pos + 4 > words.size()) {
          ++native_bad_;
          f.pos = words.size();
          break;
        }
        // The ring position to +4, then the fence to +0, big-endian, as the
        // two EVENT_WRITE_SHDs land.
        StoreBig(w[1] + 4, w[2]);
        StoreBig(w[1], w[3]);
        native_d3d::NotifyFenceCompleted();
        ++native_fences_;
        f.pos += 4;
        break;
      case native_d3d::kOpPresent: {
        if (f.pos + 6 > words.size()) {
          ++native_bad_;
          f.pos = words.size();
          break;
        }
        f.pos += 6;
        if (backend.tick_vblank_) backend.tick_vblank_();
        // As the ring had it: a history slot stamped with the vblank counter,
        // the swap, the counter moving on, the second slot.
        if (w[4]) StoreBig(w[4], backend.counter());
        native_presenting_ = true;
        renut_note_guest_swap();
        backend.swap.IssueSwap(w[1], w[2], w[3]);
        native_presenting_ = false;
        backend.increment_counter();
        if (w[5]) StoreBig(w[5], backend.counter());
        ++native_presents_;
        if (native_presents_ <= 3 || native_presents_ % 600 == 0) {
          REXLOG_INFO("Renut: native present {} ({:08X} {}x{}); fences {}, callbacks {}",
                      native_presents_, w[1], w[2], w[3], native_fences_, native_callbacks_);
        }
        break;
      }
      case native_d3d::kOpStore:
        if (f.pos + 3 > words.size()) {
          ++native_bad_;
          f.pos = words.size();
          break;
        }
        StoreBig(w[1], w[2]);
        native_d3d::NotifyFenceCompleted();
        f.pos += 3;
        break;
      case native_d3d::kOpCallback: {
        if (f.pos + 4 > words.size()) {
          ++native_bad_;
          f.pos = words.size();
          break;
        }
        f.pos += 4;
        ++native_callbacks_;
        if (backend.tick_vblank_) backend.tick_vblank_();
        if (!backend.dispatch_callback_) break;
        for (uint32_t cpu = 0; cpu < 6; ++cpu) {
          if ((w[3] >> cpu) & 1) backend.dispatch_callback_(w[1], w[2], cpu);
        }
        // The swap callback counts the swaps presented, which a thread waiting in D3DDevice_Swap sleeps for.
        native_d3d::NotifyFenceCompleted();
        break;
      }
      case native_d3d::kOpStreams: {
        const uint32_t mask = (w[0] >> 8) & 0x3FFFF;
        const size_t n = 1 + 2 * size_t(__builtin_popcount(mask));
        if (f.pos + n > words.size()) {
          ++native_bad_;
          f.pos = words.size();
          break;
        }
        const uint32_t* q = w + 1;
        for (uint32_t stream = 0; stream < 18; ++stream) {
          if (!((mask >> stream) & 1)) continue;
          backend.draw.native_streams_[stream] = {q[0], q[1]};
          q += 2;
        }
        f.pos += n;
        break;
      }
      case native_d3d::kOpWaitMemory: {
        if (f.pos + 4 > words.size()) {
          ++native_bad_;
          f.pos = words.size();
          break;
        }
        f.pos += 4;
        // As the GPU would, until the vertical-blank handler (another thread)
        // writes the value; never longer than a few frames.
        const auto* word = backend.memory_->TranslatePhysical<const uint32_t*>(w[1]);
        const auto start = std::chrono::steady_clock::now();
        ++backend.targets.native_waits_;
        while ((__builtin_bswap32(__atomic_load_n(word, __ATOMIC_ACQUIRE)) & w[3]) != w[2]) {
          if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(100)) {
            ++backend.targets.native_wait_timeouts_;
            break;
          }
          std::this_thread::sleep_for(std::chrono::microseconds(250));
        }
        backend.targets.native_wait_us_ += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - start)
                                        .count());
        break;
      }
      case native_d3d::kOpIndices:
        if (f.pos + 3 > words.size()) {
          ++native_bad_;
          f.pos = words.size();
          break;
        }
        native_indices_ = {w[1], w[2]};
        f.pos += 3;
        break;
      case native_d3d::kOpSurfaces: {
        const uint32_t slots = (w[0] >> 8) & 0x1F;
        const size_t n = 1 + 4 * size_t(__builtin_popcount(slots));
        if (f.pos + n > words.size()) {
          ++native_bad_;
          f.pos = words.size();
          break;
        }
        const uint32_t* p = w + 1;
        for (uint32_t slot = 0; slot < 5; ++slot) {
          if (!((slots >> slot) & 1)) continue;
          backend.targets.native_surfaces_[slot] = {p[0], p[1], p[2], p[3]};
          p += 4;
        }
        f.pos += n;
        break;
      }
      default:
        ++native_bad_;
        f.pos = words.size();
        break;
    }
  }
  return found;
}

// A clear record: Direct3D's clear of the current targets, without its draw.
// The targets are the bound surfaces', and only the clear's rectangle is
// cleared.
void Lists::NativeClear(const uint32_t* w) {
  backend.recorder.FlushDeferred();
  if (backend.skip_mask_ & 134217728) return;
  const uint32_t flags = w[0] >> 8, present = w[11];
  const int32_t x1 = int32_t(w[1]), y1 = int32_t(w[2]), x2 = int32_t(w[3]), y2 = int32_t(w[4]);
  float color[4], z;
  std::memcpy(color, w + 5, sizeof(color));
  std::memcpy(&z, w + 9, sizeof(z));
  const uint32_t stencil = w[10] & 0xFF;
  ++backend.stats_.clears;
  auto clip = [&](const Target& t) {
    const auto low = [&](int32_t v) { return int32_t(HostFloor(&t, uint32_t(std::max(v, 0)))); };
    const auto high = [&](int32_t v) { return int32_t(HostCeil(&t, uint32_t(std::max(v, 0)))); };
    return RenderRect(std::clamp(low(x1), 0, int32_t(t.width)), std::clamp(low(y1), 0, int32_t(t.height)),
                      std::clamp(high(x2), 0, int32_t(t.width)), std::clamp(high(y2), 0, int32_t(t.height)));
  };
  backend.draw.Trace("native clear flags={:X} present={:X} rect=({},{})-({},{}) color=({}, {}, {}, {}) z={} s={}", flags,
        present, x1, y1, x2, y2, color[0], color[1], color[2], color[3], z, stencil);
  std::array<Target*, 4> cleared{};
  for (uint32_t i = 0; i < 4; ++i) {
    if (!((flags >> i) & 1) || !((present >> i) & 1)) continue;
    Target* target = backend.targets.NativeTarget(i);
    if (!target || std::find(cleared.begin(), cleared.end(), target) != cleared.end()) continue;
    cleared[i] = target;
    const RenderRect rect = clip(*target);
    if (rect.right <= rect.left || rect.bottom <= rect.top) continue;
    backend.targets.Transition(target->texture.get(), target->layout, RenderTextureLayout::COLOR_WRITE);
    std::array<Target*, 4> colors{target};
    backend.targets.SetFramebuffer(backend.targets.GetFramebuffer(colors, 1, nullptr));
    backend.recorder.L()->clearColor(0, RenderColor(color[0], color[1], color[2], color[3]), &rect, 1);
  }
  const bool clear_depth = flags & 0x10, clear_stencil = flags & 0x20;
  if ((clear_depth || clear_stencil) && (present & 0x10)) {
    if (Target* depth = backend.targets.NativeTarget(4)) {
      const RenderRect rect = clip(*depth);
      if (rect.right > rect.left && rect.bottom > rect.top) {
        // A surface whose image went to a depth resolve (AliasDepthResolve): a
        // clear of the whole depth needs nothing of the old contents - the
        // stencil comes up undefined in the other image, so it is cleared too -
        // and a smaller one or a stencil-only clear needs them back.
        bool fresh = false;
        if (depth->restore_key || depth->aliased_since_clear || depth->depth_undefined) {
          const bool whole = clear_depth && rect.left <= 0 && rect.top <= 0 && rect.right >= int32_t(depth->width) &&
                             rect.bottom >= int32_t(depth->height);
          backend.targets.NoteAliasedUse(*depth, whole);
          if (depth->restore_key) fresh = backend.targets.RestoreAliasedDepth(*depth, whole);
          if (depth->depth_undefined) {
            // The contents were given up (AliasDepthResolve): the stencil is
            // undefined as well, which a whole clear settles.
            fresh = fresh || whole;
            depth->depth_undefined = !whole;
          }
        }
        backend.targets.Transition(depth->texture.get(), depth->layout, RenderTextureLayout::DEPTH_WRITE);
        std::array<Target*, 4> none{};
        backend.targets.SetFramebuffer(backend.targets.GetFramebuffer(none, 0, depth));
        backend.recorder.L()->clearDepthStencil(clear_depth, clear_stencil || fresh, std::clamp(z, 0.f, 1.f), stencil, &rect, 1);
      }
    }
  }
}

}  // namespace renut::gpu
