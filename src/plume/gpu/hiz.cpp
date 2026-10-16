// Hi-Z culling.
#include "internal.h"

namespace renut::gpu {

#include "plume/shaders/hiz_reduce.comp.inc"
#include "plume/shaders/hiz_mips.comp.inc"
#include "plume/shaders/hiz_test.comp.inc"

void HiZ::HizIndexBox(uint64_t key, const HizBox& box) {
  auto add = [&](uint32_t base, uint32_t size) {
    if (!size) return;
    for (uint32_t block = base >> 16; block <= (base + size - 1) >> 16; ++block) {
      hiz_box_blocks_.emplace(block, key);
    }
  };
  add(box.vertex_base, box.vertex_size);
  add(box.index_base, box.index_size);
  // Entries of dropped boxes pile up; rebuilt from the live ones now and then.
  if (hiz_box_blocks_.size() > 400000) {
    hiz_box_blocks_.clear();
    for (const auto& [k, b] : hiz_boxes_) {
      if (b.vertex_size) {
        for (uint32_t block = b.vertex_base >> 16; block <= (b.vertex_base + b.vertex_size - 1) >> 16; ++block) {
          hiz_box_blocks_.emplace(block, k);
        }
      }
      if (b.index_size) {
        for (uint32_t block = b.index_base >> 16; block <= (b.index_base + b.index_size - 1) >> 16; ++block) {
          hiz_box_blocks_.emplace(block, k);
        }
      }
    }
  }
}

// The guest wrote [start, start + length): the boxes made from any of it go.
void HiZ::HizDropBoxes(uint32_t start, uint32_t length) {
  if (!length) return;
  ++hiz_box_generation_;
  const uint32_t first = start >> 16, last = uint32_t((uint64_t(start) + length - 1) >> 16);
  if (last - first > 4096) {
    // A huge range: cheaper to look at every box.
    for (auto it = hiz_boxes_.begin(); it != hiz_boxes_.end();) {
      const HizBox& box = it->second;
      const bool hit = (box.vertex_size && box.vertex_base < uint64_t(start) + length &&
                        start < box.vertex_base + box.vertex_size) ||
                       (box.index_size && box.index_base < uint64_t(start) + length &&
                        start < box.index_base + box.index_size);
      it = hit ? hiz_boxes_.erase(it) : std::next(it);
    }
    return;
  }
  for (uint32_t block = first; block <= last; ++block) {
    auto range = hiz_box_blocks_.equal_range(block);
    for (auto it = range.first; it != range.second;) {
      hiz_boxes_.erase(it->second);
      it = hiz_box_blocks_.erase(it);
    }
  }
}

HizModelEntry* HiZ::HizModelFor(HostShader& vs) {
  if (!vs.bool_mask_decoded) {
    rex::string::StringBuffer disassembly;
    vs.AnalyzeUcode(disassembly);
    std::memcpy(vs.bool_mask, vs.constant_register_map().bool_bitmap, sizeof(vs.bool_mask));
    vs.bool_mask_decoded = true;
  }
  const gfx::RegisterFile& regs = *backend.register_file_;
  uint32_t bools[8];
  bool same = vs.hiz_entry != nullptr;
  for (uint32_t word = 0; word < 8; ++word) {
    bools[word] = regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + word] & vs.bool_mask[word];
    same = same && bools[word] == vs.hiz_bools[word];
  }
  if (same) return static_cast<HizModelEntry*>(vs.hiz_entry);
  uint64_t key = vs.ucode_data_hash() * 0x9E3779B97F4A7C15ull;
  for (uint32_t word = 0; word < 8; ++word) key = (key ^ bools[word]) * 0x100000001B3ull;
  auto it = hiz_models_.find(key);
  if (it != hiz_models_.end()) {
    vs.hiz_entry = &it->second;
    std::memcpy(vs.hiz_bools, bools, sizeof(bools));
    return &it->second;
  }
  const auto start = std::chrono::steady_clock::now();
  HizModelEntry entry;
  entry.model = renut::hiz::BuildModel(vs.ucode_dwords(), vs.ucode_dword_count(), bools);
  ++backend.stats_.hiz_models;
  if (entry.model->valid) ++backend.stats_.hiz_models_valid;
  backend.stats_.hiz_model_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - start).count());
  HizModelEntry* made = &hiz_models_.emplace(key, std::move(entry)).first->second;
  vs.hiz_entry = made;
  std::memcpy(vs.hiz_bools, bools, sizeof(bools));
  return made;
}

// The model against the SDK's interpreter, on two vertices of the draw about to
// use it: what the symbolic executor claims must be what the microcode does.
bool HiZ::HizValidate(HostShader& vs, renut::hiz::Model& model,
                                        const xenos::xe_gpu_vertex_fetch_t& fetch, uint32_t vertex_a,
                                        uint32_t vertex_b) {
  if (!gfx::ShaderInterpreter::CanInterpretShader(vs)) return false;
  struct Sink final : gfx::ShaderInterpreter::ExportSink {
    float position[4] = {};
    uint32_t written = 0;
    void Export(rex::graphics::ucode::ExportRegister reg, const float* value, uint32_t mask) override {
      if (reg != rex::graphics::ucode::ExportRegister::kVSPosition) return;
      for (uint32_t i = 0; i < 4; ++i) {
        if (mask & (1u << i)) {
          position[i] = value[i];
          written |= 1u << i;
        }
      }
    }
  } sink;
  auto interpreter = std::make_unique<gfx::ShaderInterpreter>(*backend.register_file_, *backend.memory_);
  interpreter->SetShader(vs);
  interpreter->SetExportSink(&sink);
  const float* constants = reinterpret_cast<const float*>(&backend.register_file_->values[XE_GPU_REG_SHADER_CONSTANT_000_X]);
  float affine[4][5];
  renut::hiz::Evaluate(model, constants, 0, affine);
  const uint8_t* memory = backend.memory_->TranslatePhysical<const uint8_t*>(0);
  for (uint32_t vertex : {vertex_a, vertex_b}) {
    sink.written = 0;
    interpreter->temp_registers()[0] = float(vertex);
    interpreter->Execute();
    float p[4] = {};
    if (!renut::hiz::FetchVertex(model.fetch, memory, fetch.address, fetch.size, uint32_t(fetch.endian), vertex, p)) {
      return false;
    }
    if (sink.written != 0xF) return false;
    for (int o = 0; o < 4; ++o) {
      float predicted = affine[o][0];
      for (int j = 0; j < 4; ++j) predicted += affine[o][1 + j] * p[j];
      const float reference = sink.position[o];
      if (!(std::fabs(predicted - reference) <= 2e-3f * (1.0f + std::fabs(predicted) + std::fabs(reference)))) {
        static uint32_t logged = 0;
        if (logged < 12) {
          ++logged;
          REXLOG_WARN("Renut: Plume Hi-Z model of vs {:016X} rejected: vertex {} clip[{}] model {} interpreter {}",
                      vs.ucode_data_hash(), vertex, o, predicted, reference);
        }
        return false;
      }
    }
  }
  return true;
}

const HizBox* HiZ::HizBoxFor(
    const renut::hiz::Model& model, const xenos::xe_gpu_vertex_fetch_t& fetch, const IndexBufferInfo* index_info,
    uint32_t index_count, int32_t base_vertex) {
  backend.streams.DrainWatchHits();
  const bool indexed = index_info && index_info->guest_base;
  const renut::hiz::FetchDesc& f = model.fetch;
  uint64_t parts[16];
  uint32_t n = 0;
  parts[n++] = (uint64_t(fetch.address) << 32) | fetch.size;
  parts[n++] = (uint64_t(uint32_t(fetch.endian)) << 56) | (uint64_t(f.fetch_constant) << 48) |
               (uint64_t(f.format) << 40) | (uint64_t(f.is_signed) << 32) | (uint64_t(f.normalized) << 33) |
               (uint64_t(f.signed_no_zero) << 34) | (uint64_t(f.used_mask) << 36) |
               uint64_t(uint32_t(f.exp_adjust) & 0x3F);
  parts[n++] = (uint64_t(f.stride_dwords) << 32) | uint32_t(f.offset_dwords);
  parts[n++] = indexed ? ((uint64_t(index_info->guest_base) << 32) | index_count) : index_count;
  parts[n++] = indexed ? ((uint64_t(uint32_t(index_info->format)) << 8) | uint64_t(uint32_t(index_info->endianness)))
                       : 0xFFFF;
  parts[n++] = uint64_t(uint32_t(base_vertex));
  // A multiply-and-rotate mix of the six words: the key only has to tell two
  // meshes apart, and this ran for every draw.
  uint64_t key = 0x9E3779B97F4A7C15ull;
  for (uint32_t i = 0; i < n; ++i) {
    key = (key ^ parts[i]) * 0xFF51AFD7ED558CCDull;
    key ^= key >> 32;
  }
  hiz_last_box_key_ = key;
  HizFront& front = hiz_front_[key & (hiz_front_.size() - 1)];
  if (front.key == key && front.generation == hiz_box_generation_ && front.box &&
      backend.swaps_ - front.box->checked_frame < 30) {
    ++backend.stats_.hiz_box_hits;
    return front.box->valid ? front.box : nullptr;
  }

  const uint8_t* memory = backend.memory_->TranslatePhysical<const uint8_t*>(0);
  auto checksum_of = [&](const HizBox& box) {
    uint64_t h = 0;
    auto add = [&](uint32_t base, uint32_t size) {
      if (!size) return;
      const uint32_t head = std::min<uint32_t>(size, 128);
      h = XXH3_64bits_withSeed(memory + base, head, h);
      if (size > head) h = XXH3_64bits_withSeed(memory + base + size - head, head, h);
    };
    add(box.vertex_base, box.vertex_size);
    add(box.index_base, box.index_size);
    return h;
  };
  auto found = hiz_boxes_.find(key);
  if (found != hiz_boxes_.end()) {
    HizBox& box = found->second;
    // The write watch drops a box when the guest writes under it; the sample of
    // its bytes is a second line, looked at every 30 frames rather than at every draw.
    if (backend.swaps_ - box.checked_frame < 30) {
      ++backend.stats_.hiz_box_hits;
      front = {key, &box, hiz_box_generation_};
      return box.valid ? &box : nullptr;
    }
    if (checksum_of(box) == box.checksum) {
      box.checked_frame = backend.swaps_;
      ++backend.stats_.hiz_box_hits;
      front = {key, &box, hiz_box_generation_};
      return box.valid ? &box : nullptr;
    }
    hiz_boxes_.erase(found);
    ++hiz_box_generation_;
  }

  const auto start = std::chrono::steady_clock::now();
  HizBox box;
  box.vertex_base = fetch.address << 2;
  box.vertex_size = fetch.size << 2;
  const bool is32 = indexed && index_info->format == xenos::IndexFormat::kInt32;
  if (indexed) {
    box.index_base = index_info->guest_base;
    box.index_size = index_count * (is32 ? 4 : 2);
  }
  // `cache` false hands the box back through hiz_scratch_box_ instead of the map.
  auto finish = [&](bool valid, bool cache) -> const HizBox* {
    backend.stats_.hiz_box_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - start).count());
    ++backend.stats_.hiz_boxes_made;
    box.valid = valid;
    if (!valid && !cache) return nullptr;
    if (!cache) {
      hiz_scratch_box_ = box;
      return &hiz_scratch_box_;
    }
    box.checksum = checksum_of(box);
    box.checked_frame = backend.swaps_;
    const HizBox* stored = &hiz_boxes_.emplace(key, box).first->second;
    HizIndexBox(key, box);
    front = {key, stored, hiz_box_generation_};
    return valid ? stored : nullptr;
  };
  if (!box.vertex_size || uint64_t(box.vertex_base) + box.vertex_size > 0x20000000ull ||
      (indexed && (uint64_t(box.index_base) + box.index_size > 0x20000000ull || !index_count))) {
    return finish(false, false);
  }
  // Index range.
  int64_t lowest = INT64_MAX, highest = -1;
  if (indexed) {
    const uint32_t* words = reinterpret_cast<const uint32_t*>(memory + box.index_base);
    const uint16_t* halves = reinterpret_cast<const uint16_t*>(memory + box.index_base);
    xenos::Endian endian = index_info->endianness;
    if (!is32) {
      if (endian == xenos::Endian::k8in32) endian = xenos::Endian::k8in16;
      else if (endian == xenos::Endian::k16in32) endian = xenos::Endian::kNone;
    }
    for (uint32_t i = 0; i < index_count; ++i) {
      uint32_t v = is32 ? words[i] : halves[i];
      v = xenos::GpuSwap(v, endian);
      if (!is32) v &= 0xFFFF;
      if (v == (is32 ? 0xFFFFFFFFu : 0xFFFFu)) continue;  // primitive restart
      v &= 0xFFFFFF;
      lowest = std::min<int64_t>(lowest, v);
      highest = std::max<int64_t>(highest, v);
    }
    if (highest < 0) return finish(false, true);
  } else {
    lowest = 0;
    highest = int64_t(index_count) - 1;
  }
  const int64_t first = lowest + base_vertex, last = highest + base_vertex;
  if (first < 0 || last - first > 20000) return finish(false, true);
  float lo[4] = {FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX}, hi[4] = {-FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX};
  for (int64_t v = first; v <= last; ++v) {
    float p[4];
    if (!renut::hiz::FetchVertex(f, memory, fetch.address, fetch.size, uint32_t(fetch.endian), uint32_t(v), p)) {
      return finish(false, true);
    }
    for (int j = 0; j < 4; ++j) {
      if (!std::isfinite(p[j])) return finish(false, true);
      lo[j] = std::min(lo[j], p[j]);
      hi[j] = std::max(hi[j], p[j]);
    }
  }
  std::memcpy(box.lo, lo, sizeof(lo));
  std::memcpy(box.hi, hi, sizeof(hi));
  box.first_vertex = uint32_t(first);
  box.last_vertex = uint32_t(last);
  // What the box was made from must be watched, or a guest rewrite of the mesh
  // (streaming, a dynamic buffer) would leave it stale: the checksum above only
  // samples the ends.
  const auto access = [&](uint32_t base, uint32_t size) {
    return !size || backend.memory_->GetPhysicalHeap()->QueryRangeAccess(base, base + size - 1) ==
                       rex::memory::PageAccess::kReadWrite;
  };
  if (!access(box.vertex_base, box.vertex_size) || !access(box.index_base, box.index_size)) {
    // Made again for the next draw; the range is bounded above (20000 vertices).
    return finish(true, false);
  }
  backend.memory_->EnablePhysicalMemoryAccessCallbacks(box.vertex_base, box.vertex_size, true, false);
  if (box.index_size) backend.memory_->EnablePhysicalMemoryAccessCallbacks(box.index_base, box.index_size, true, false);
  return finish(true, true);
}

bool HiZ::HizCullDraw(HostShader& vs, xenos::PrimitiveType prim_type, uint32_t index_count,
                                        const IndexBufferInfo* index_info, bool color_pass) {
  const uint32_t kind = color_pass ? 1 : 0;
  backend.draw.hiz_test_.valid = false;
  ++backend.stats_.hiz_seen[kind];
  if (!index_count) return ++backend.stats_.hiz_why[0], false;
  switch (prim_type) {
    case xenos::PrimitiveType::kTriangleList:
    case xenos::PrimitiveType::kTriangleStrip:
    case xenos::PrimitiveType::kTriangleFan:
    case xenos::PrimitiveType::kQuadList: break;
    default: return ++backend.stats_.hiz_why[1], false;
  }
  const gfx::RegisterFile& regs = *backend.register_file_;
  if (vs.entry && (vs.entry->specConstantsMask & RENUT_SHADER_FLAG_MEMEXPORT)) return ++backend.stats_.hiz_why[2], false;
  const auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  const auto clip = regs.Get<reg::PA_CL_CLIP_CNTL>();
  // vtx_w0_fmt set is the usual case: the shader's W is W, not 1/W.
  if (clip.clip_disable || vte.vtx_xy_fmt || vte.vtx_z_fmt || !vte.vtx_w0_fmt || !vte.vport_x_scale_ena ||
      !vte.vport_x_offset_ena || !vte.vport_y_scale_ena || !vte.vport_y_offset_ena) {
    ++backend.report.hiz_clip_states_[(uint64_t(vte.value) << 32) | clip.value];
    return ++backend.stats_.hiz_why[3], false;
  }
  if (regs.Get<reg::SQ_VS_CONST>(XE_GPU_REG_SQ_VS_CONST).base != 0) return ++backend.stats_.hiz_why[4], false;

  const auto stage_a = hiz_stage_timing_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
  HizModelEntry* entry = HizModelFor(vs);
  if (hiz_stage_timing_) {
    backend.stats_.hiz_stage_ns[0] += 16 * uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::steady_clock::now() - stage_a).count());
  }
  if (!entry->model || !entry->model->valid) {
    ++backend.stats_.hiz_no_model[kind];
    ++backend.stats_.hiz_why_model[kind][entry->model ? uint32_t(entry->model->why) % 12 : 0];
    ++backend.report.hiz_nomodel_draws_[(vs.entry ? vs.entry->hash : 0) ^ (entry->model ? uint64_t(entry->model->why) & 15 : 0)];
    backend.report.hiz_nomodel_why_[vs.entry ? vs.entry->hash : 0] = entry->model ? uint32_t(entry->model->why) : 0;
    return false;
  }
  renut::hiz::Model& model = *entry->model;
  const xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(model.fetch.fetch_constant);
  if (fetch.type != xenos::FetchConstantType::kVertex || !fetch.size) {
    ++backend.stats_.hiz_no_box[kind];
    return false;
  }
  const int32_t base_vertex = int32_t(regs[XE_GPU_REG_VGT_INDX_OFFSET]);
  // A mesh whose draws are almost never found outside (under 1 in 16, over the
  // last 64-128 draws of it, every instance together) costs more in tests than
  // the few culls save: break-even is about one cull in ten. Only every 8th draw
  // of it is tested; a cull found there counts for 8, so the estimate stays
  // unbiased and a mesh that starts to leave the view is tested again within a
  // few draws. Skipping a test is always safe: the draw is just made. The mesh
  // is named by a cheap key (model, fetch range, index range), so a skipped draw
  // never reaches the box lookup either.
  HizCold* cold_slot = nullptr;
  bool sampled_cold = false;
  if (backend.swap.hiz_skip_ && backend.report.hiz_mode_ == 2) {
    const uint64_t index_word = index_info && index_info->guest_base
                                    ? (uint64_t(index_info->guest_base) << 32) | index_count
                                    : uint64_t(index_count);
    uint64_t k = (uint64_t(reinterpret_cast<uintptr_t>(entry)) ^ 0x9E3779B97F4A7C15ull) * 0xFF51AFD7ED558CCDull;
    k = (k ^ ((uint64_t(fetch.address) << 32) | fetch.size)) * 0xC4CEB9FE1A85EC53ull;
    k = (k ^ index_word ^ (uint64_t(uint32_t(base_vertex)) << 40)) * 0xFF51AFD7ED558CCDull;
    k ^= k >> 29;
    cold_slot = &hiz_cold_[k & (hiz_cold_.size() - 1)];
    if (cold_slot->key != k) *cold_slot = HizCold{k, 0, 0, 0};
    if (cold_slot->seen >= 128) {
      cold_slot->seen >>= 1;
      cold_slot->culled >>= 1;
    }
    const bool cold = cold_slot->seen >= 32 && cold_slot->culled * 16 < cold_slot->seen;
    ++cold_slot->seen;
    if (cold) {
      if ((++cold_slot->phase & 7) != 0) {
        ++backend.stats_.hiz_skipped[kind];
        return false;
      }
      sampled_cold = true;
    }
  }
  const auto stage_b = hiz_stage_timing_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
  const HizBox* box = HizBoxFor(model, fetch, index_info, index_count, base_vertex);
  if (hiz_stage_timing_) {
    backend.stats_.hiz_stage_ns[1] += 16 * uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::steady_clock::now() - stage_b).count());
  }
  if (!box) {
    ++backend.stats_.hiz_no_box[kind];
    return false;
  }
  if (!entry->validated) {
    // First use: the first and the last vertex of the draw through the SDK's
    // interpreter against the model.
    if (!HizValidate(vs, model, fetch, box->first_vertex, box->last_vertex)) {
      model.valid = false;
      ++backend.stats_.hiz_model_rejected;
      ++backend.stats_.hiz_no_model[kind];
      return false;
    }
    entry->validated = true;
  }
  // The constants as the shader sees them, and the model's affine map.
  const float* constants = reinterpret_cast<const float*>(&regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X]);
  float affine[4][5];
  const auto stage_c = hiz_stage_timing_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
  renut::hiz::Evaluate(model, constants, 0, affine);
  if (hiz_stage_timing_) {
    backend.stats_.hiz_stage_ns[2] += 16 * uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::steady_clock::now() - stage_c).count());
  }
  struct StageGuard {
    uint64_t* out;
    std::chrono::steady_clock::time_point begin;
    bool on;
    ~StageGuard() {
      if (on) {
        *out += 16 * uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - begin).count());
      }
    }
  } plane_stage{&backend.stats_.hiz_stage_ns[3],
                hiz_stage_timing_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point(),
                hiz_stage_timing_};
  // The box in clip space is a parallelepiped: centre C and, along the element's
  // components, the columns A[.][1 + j] * half_j. A plane's value over it ranges
  // over centre +- sum |coefficient along j| * half_j, so "all of it outside" is
  // one comparison per plane and no corner is formed.
  float centre[4], half[4], mid[4];
  uint32_t comps[4], ncomps = 0;
  for (uint32_t j = 0; j < 4; ++j) {
    mid[j] = 0.5f * (box->lo[j] + box->hi[j]);
    half[j] = 0.5f * (box->hi[j] - box->lo[j]);
    if (!(model.fetch.used_mask & (1u << j))) continue;
    bool any = false;
    for (int o = 0; o < 4; ++o) any = any || affine[o][1 + j] != 0.f;
    if (any) comps[ncomps++] = j;
  }
  float magnitude = 0.f;
  for (int o = 0; o < 4; ++o) {
    float c = affine[o][0];
    for (uint32_t i = 0; i < ncomps; ++i) {
      const uint32_t j = comps[i];
      c += affine[o][1 + j] * mid[j];
      magnitude += std::fabs(affine[o][1 + j] * half[j]);
    }
    if (!std::isfinite(c)) return false;
    centre[o] = c;
    magnitude += std::fabs(c);
  }
  ++backend.stats_.hiz_eligible[kind];
  // [lowest, highest] of the plane s . clip over the box.
  auto plane_range = [&](float sx, float sy, float sz, float sw, float* lowest, float* highest) {
    float c = sx * centre[0] + sy * centre[1] + sz * centre[2] + sw * centre[3];
    float r = 0.f;
    for (uint32_t i = 0; i < ncomps; ++i) {
      const uint32_t j = comps[i];
      r += std::fabs(sx * affine[0][1 + j] + sy * affine[1][1 + j] + sz * affine[2][1 + j] + sw * affine[3][1 + j]) *
           half[j];
    }
    *lowest = c - r;
    *highest = c + r;
  };
  const float slack = 1e-4f * magnitude;
  auto wholly_outside = [&](float sx, float sy, float sz, float sw) {
    float low, high;
    plane_range(sx, sy, sz, sw, &low, &high);
    return high < -slack;
  };
  if (wholly_outside(1, 0, 0, 1) || wholly_outside(-1, 0, 0, 1) || wholly_outside(0, 1, 0, 1) ||
      wholly_outside(0, -1, 0, 1) ||
      (clip.dx_clip_space_def && (wholly_outside(0, 0, 1, 0) || wholly_outside(0, 0, -1, 1)))) {
    ++backend.stats_.hiz_frustum[kind];
    if (cold_slot) cold_slot->culled += sampled_cold ? 8 : 1;
    return backend.report.hiz_mode_ >= 2;
  }
  // Wholly in front of the eye: the scissor and the screen rectangle make sense.
  float w_low, w_high;
  plane_range(0, 0, 0, 1, &w_low, &w_high);
  if (!(w_low > 1e-6f)) return false;
  float window_x = 0.f, window_y = 0.f;
  if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
    const auto offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    window_x = float(offset.window_x_offset);
    window_y = float(offset.window_y_offset);
  }
  if (regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero) {
    window_x += 0.5f;
    window_y += 0.5f;
  }
  const float xs = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE);
  const float xo = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET);
  const float ys = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE);
  const float yo = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET);
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  const float left = float(scissor.offset[0]), top = float(scissor.offset[1]);
  const float right = left + float(scissor.extent[0]), bottom = top + float(scissor.extent[1]);
  // x_screen = xo + window_x + xs * x / w. All of the box left of (left - 1):
  // (left - 1 - xo - window_x) * w - xs * x > 0 everywhere, and so on.
  auto all_positive = [&](float sx, float sy, float sw) {
    float low, high;
    plane_range(sx, sy, 0, sw, &low, &high);
    return low > slack;
  };
  const float bx = xo + window_x, by = yo + window_y;
  if (all_positive(-xs, 0, left - 1.0f - bx) || all_positive(xs, 0, -(right + 1.0f - bx)) ||
      all_positive(0, -ys, top - 1.0f - by) || all_positive(0, ys, -(bottom + 1.0f - by))) {
    ++backend.stats_.hiz_scissor[kind];
    if (cold_slot) cold_slot->culled += sampled_cold ? 8 : 1;
    return backend.report.hiz_mode_ >= 2;
  }
  if (backend.report.hiz_mode_ >= 3 && clip.dx_clip_space_def) {
    // The occlusion test wants the rectangle and the nearest depth: corners.
    const float zs = vte.vport_z_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE) : 1.f;
    const float zo = vte.vport_z_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET) : 0.f;
    const float depth_min = std::clamp(zo, 0.f, 1.f), depth_max = std::clamp(zo + zs, 0.f, 1.f);
    float min_x = FLT_MAX, max_x = -FLT_MAX, min_y = FLT_MAX, max_y = -FLT_MAX, nearest = FLT_MAX;
    const uint32_t corners = 1u << ncomps;
    for (uint32_t c = 0; c < corners; ++c) {
      float clip_v[4];
      for (int o = 0; o < 4; ++o) {
        float v = affine[o][0];
        for (uint32_t i = 0; i < ncomps; ++i) {
          const uint32_t j = comps[i];
          v += affine[o][1 + j] * (((c >> i) & 1) ? box->hi[j] : box->lo[j]);
        }
        clip_v[o] = v;
      }
      const float inv = 1.0f / clip_v[3];
      const float x = bx + xs * clip_v[0] * inv;
      const float y = by + ys * clip_v[1] * inv;
      min_x = std::min(min_x, x);
      max_x = std::max(max_x, x);
      min_y = std::min(min_y, y);
      max_y = std::max(max_y, y);
      const float z = std::clamp(clip_v[2] * inv, 0.f, 1.f);
      nearest = std::min(nearest, depth_min + z * (depth_max - depth_min));
    }
    backend.draw.hiz_test_.valid = true;
    // The rectangle is also cut to the scissor: nothing outside it is drawn.
    backend.draw.hiz_test_.rect[0] = std::max(min_x - 1.0f, left);
    backend.draw.hiz_test_.rect[1] = std::max(min_y - 1.0f, top);
    backend.draw.hiz_test_.rect[2] = std::min(max_x + 1.0f, right);
    backend.draw.hiz_test_.rect[3] = std::min(max_y + 1.0f, bottom);
    backend.draw.hiz_test_.z = nearest;
    backend.draw.hiz_test_.key = hiz_last_box_key_ ^ (vs.ucode_data_hash() * 0x9E3779B97F4A7C15ull);
  }
  return false;
}

// ---------------------------------------------------------------------------
// Hi-Z: occlusion by the depth the pass has drawn so far
//
// A window of deferred draws may be recorded in any order (see IssueDraw). The
// draws a window holds that were wholly hidden the last time they were tested
// are kept back; the rest are recorded as usual. Then, with the pass closed, the
// depth image is copied out, reduced to a pyramid of its farthest values, and
// every boxed draw of the window is tested against it on the GPU (a rectangle
// and the box's nearest depth). The kept-back draws are recorded last, each
// inside conditional rendering on its own answer: a draw that is still hidden
// costs the GPU next to nothing, one that is not hidden runs - in the same
// frame, so nothing arrives late and nothing can be wrong by a frame. What the
// tests found feeds the prediction for the frames after.
// ---------------------------------------------------------------------------

bool HiZ::HizCreatePipelines() {
  if (hiz_pipelines_[2] != VK_NULL_HANDLE) return true;
  if (hiz_pipelines_failed_) return false;
  hiz_pipelines_failed_ = true;
  VkDevice vk_device = static_cast<VulkanDevice*>(backend.device_)->vk;
  if (hiz_layout_ == VK_NULL_HANDLE) {
    VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
    VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(vk_device, &layout_info, nullptr, &hiz_layout_) != VK_SUCCESS) return false;
  }
  const std::pair<const uint32_t*, size_t> code[3] = {{k_hiz_reduce_comp, sizeof(k_hiz_reduce_comp)},
                                                      {k_hiz_mips_comp, sizeof(k_hiz_mips_comp)},
                                                      {k_hiz_test_comp, sizeof(k_hiz_test_comp)}};
  for (uint32_t i = 0; i < 3; ++i) {
    if (hiz_pipelines_[i] != VK_NULL_HANDLE) continue;
    VkShaderModuleCreateInfo module_info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_info.codeSize = code[i].second;
    module_info.pCode = code[i].first;
    if (vkCreateShaderModule(vk_device, &module_info, nullptr, &hiz_modules_[i]) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo pipeline_info = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = hiz_modules_[i];
    pipeline_info.stage.pName = "main";
    pipeline_info.layout = hiz_layout_;
    if (vkCreateComputePipelines(vk_device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &hiz_pipelines_[i]) !=
        VK_SUCCESS) {
      hiz_pipelines_[i] = VK_NULL_HANDLE;
      REXLOG_WARN("Renut: Plume Hi-Z compute pipelines unavailable; occlusion culling stays off");
      return false;
    }
  }
  hiz_pipelines_failed_ = false;
  return true;
}

void HiZ::HizDestroy() {
  if (!backend.device_) return;
  VkDevice vk_device = static_cast<VulkanDevice*>(backend.device_)->vk;
  for (auto& pipeline : hiz_pipelines_) {
    if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(vk_device, pipeline, nullptr);
    pipeline = VK_NULL_HANDLE;
  }
  for (auto& module : hiz_modules_) {
    if (module != VK_NULL_HANDLE) vkDestroyShaderModule(vk_device, module, nullptr);
    module = VK_NULL_HANDLE;
  }
  if (hiz_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(vk_device, hiz_layout_, nullptr);
  hiz_layout_ = VK_NULL_HANDLE;
  if (hiz_query_pool_ != VK_NULL_HANDLE) vkDestroyQueryPool(vk_device, hiz_query_pool_, nullptr);
  hiz_query_pool_ = VK_NULL_HANDLE;
  hiz_pyramid_.reset();
  for (auto& buffer : hiz_pred_) buffer.reset();
  for (auto& buffer : hiz_read_) buffer.reset();
}

// What a window's test needs, made once: the pipelines, the depth copy's
// buffer (shared with the depth resolves), the pyramid, and this slot's
// predicate and readback buffers.
bool HiZ::HizEnsureResources() {
  if (!hiz_cond_ || !HizCreatePipelines()) return false;
  constexpr uint64_t kStagingSize = 4096ull * 2048 * 4;
  if (!backend.resolve.depth_staging_) {
    backend.resolve.depth_staging_ = backend.device_->createBuffer(RenderBufferDesc::DefaultBuffer(
        kStagingSize, RenderBufferFlag::STORAGE | RenderBufferFlag::CONSTANT | RenderBufferFlag::DEVICE_ADDRESSABLE));
    if (!backend.resolve.depth_staging_) return false;
  }
  if (!hiz_pyramid_) {
    // 4096x2048 reduced by 8, then halved four times: 1.34 x 512 x 256 floats.
    hiz_pyramid_ = backend.device_->createBuffer(RenderBufferDesc::DefaultBuffer(
        uint64_t(1) << 20, RenderBufferFlag::STORAGE | RenderBufferFlag::DEVICE_ADDRESSABLE));
    if (!hiz_pyramid_) return false;
  }
  if (!hiz_pred_[backend.slot_]) {
    hiz_pred_[backend.slot_] = backend.device_->createBuffer(RenderBufferDesc::DefaultBuffer(
        uint64_t(kHizMaxRecords) * 8, RenderBufferFlag::STORAGE | RenderBufferFlag::DEVICE_ADDRESSABLE));
    // The answers, then the farthest depth each test found, then a debug copy.
    hiz_read_[backend.slot_] = backend.device_->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(kHizMaxRecords) * 8 + 65536 * 4));
    if (!hiz_pred_[backend.slot_] || !hiz_read_[backend.slot_]) {
      hiz_pred_[backend.slot_].reset();
      hiz_read_[backend.slot_].reset();
      return false;
    }
  }
  return true;
}

// Tests the records [first, first + count) of hiz_records_ against `target`'s
// depth. On success the answers are in hiz_pred_[slot_] from record `base`
// (hiz_pred_used_) on, and the render pass is closed (the next draw reopens it).
bool HiZ::HizRunTest(Target& target, uint32_t count, uint32_t* base_out) {
  const uint32_t w = target.width, h = target.height;
  if (!count || uint64_t(w) * h * 4 > 4096ull * 2048 * 4 || target.format != RenderFormat::D32_FLOAT_S8_UINT) {
    return false;
  }
  if (hiz_pred_used_[backend.slot_] + count > kHizMaxRecords) return false;
  // Levels: 8 px blocks, then 16, 32, 64, 128.
  constexpr uint32_t kLevels = 5;
  uint32_t lw[kLevels], lh[kLevels], offset[kLevels];
  uint32_t total = 0;
  lw[0] = (w + 7) / 8;
  lh[0] = (h + 7) / 8;
  for (uint32_t l = 1; l < kLevels; ++l) {
    lw[l] = (lw[l - 1] + 1) / 2;
    lh[l] = (lh[l - 1] + 1) / 2;
  }
  for (uint32_t l = 0; l < kLevels; ++l) {
    offset[l] = total;
    total += lw[l] * lh[l];
  }
  if (uint64_t(total) * 4 > (uint64_t(1) << 20)) return false;
  const uint64_t records_offset = backend.streams.Allocate(uint64_t(count) * 32, 256);
  if (records_offset == UINT64_MAX) return false;
  auto* out = reinterpret_cast<float*>(backend.draw.Pointer(records_offset));
  for (uint32_t i = 0; i < count; ++i) {
    const HizRecord& r = hiz_records_[i];
    float* record = out + size_t(i) * 8;
    record[0] = r.rect[0];
    record[1] = r.rect[1];
    record[2] = r.rect[2];
    record[3] = r.rect[3];
    record[4] = r.z;
    record[5] = record[6] = record[7] = 0.f;
  }
  const uint32_t base = hiz_pred_used_[backend.slot_];
  // Closes the pass and puts the depth where it can be copied from.
  backend.recorder.L()->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(target.texture.get(), RenderTextureLayout::COPY_SOURCE));
  ++backend.stats_.barriers;
  target.layout = RenderTextureLayout::COPY_SOURCE;
  backend.targets.bound_framebuffer_ = nullptr;
  VkCommandBuffer cb = static_cast<VulkanCommandList*>(backend.recorder.L())->vk;
  VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
  VkBufferImageCopy region = {};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = {w, h, 1};
  vkCmdCopyImageToBuffer(cb, static_cast<VulkanTexture*>(target.texture.get())->vk,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<VulkanBuffer*>(backend.resolve.depth_staging_.get())->vk, 1,
                         &region);
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0,
                       nullptr, 0, nullptr);
  const uint64_t depth_address = backend.resolve.depth_staging_->getDeviceAddress();
  const uint64_t pyramid_address = hiz_pyramid_->getDeviceAddress();
  auto compute_barrier = [&]() {
    VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0,
                         nullptr, 0, nullptr);
  };
  // Level 0 from the depth, then each from the one before.
  {
    struct {
      uint64_t src, dst;
      uint32_t width, height, bw, bh;
    } push = {depth_address, pyramid_address + uint64_t(offset[0]) * 4, w, h, lw[0], lh[0]};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, hiz_pipelines_[0]);
    vkCmdPushConstants(cb, hiz_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cb, (lw[0] * lh[0] + 63) / 64, 1, 1);
  }
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, hiz_pipelines_[1]);
  for (uint32_t l = 1; l < kLevels; ++l) {
    compute_barrier();
    struct {
      uint64_t src, dst;
      uint32_t sw, sh, dw, dh;
    } push = {pyramid_address + uint64_t(offset[l - 1]) * 4, pyramid_address + uint64_t(offset[l]) * 4,
              lw[l - 1], lh[l - 1], lw[l], lh[l]};
    vkCmdPushConstants(cb, hiz_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cb, (lw[l] * lh[l] + 63) / 64, 1, 1);
  }
  compute_barrier();
  {
    struct {
      uint64_t records, predicate, pyramid;
      uint32_t count, levels;
      uint32_t offsets[6], widths[6], heights[6];
    } push = {};
    push.records = backend.draw.Address(records_offset);
    push.predicate = hiz_pred_[backend.slot_]->getDeviceAddress() + uint64_t(base) * 4;
    push.pyramid = pyramid_address;
    push.count = count;
    push.levels = kLevels;
    for (uint32_t l = 0; l < kLevels; ++l) {
      push.offsets[l] = offset[l];
      push.widths[l] = lw[l];
      push.heights[l] = lh[l];
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, hiz_pipelines_[2]);
    vkCmdPushConstants(cb, hiz_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cb, (count + 63) / 64, 1, 1);
  }
  // The answers go to conditional rendering, and to the readback buffer.
  {
    VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0,
                         nullptr, 0, nullptr);
  }
  VkBufferCopy copies[2] = {{uint64_t(base) * 4, uint64_t(base) * 4, uint64_t(count) * 4},
                            {uint64_t(kHizMaxRecords + base) * 4, uint64_t(kHizMaxRecords + base) * 4,
                             uint64_t(count) * 4}};
  vkCmdCopyBuffer(cb, static_cast<VulkanBuffer*>(hiz_pred_[backend.slot_].get())->vk,
                  static_cast<VulkanBuffer*>(hiz_read_[backend.slot_].get())->vk, hiz_verify_ ? 2 : 1, copies);
  // Debug: the first window of some frames also copies its level 0 out.
  if (hiz_debug_ && !hiz_debug_pending_[backend.slot_] && backend.swaps_ % 60 == 0 && lw[0] * lh[0] <= 65536) {
    VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0,
                         nullptr, 0, nullptr);
    VkBufferCopy debug_copy = {uint64_t(offset[0]) * 4, uint64_t(kHizMaxRecords) * 8, uint64_t(lw[0]) * lh[0] * 4};
    vkCmdCopyBuffer(cb, static_cast<VulkanBuffer*>(hiz_pyramid_.get())->vk,
                    static_cast<VulkanBuffer*>(hiz_read_[backend.slot_].get())->vk, 1, &debug_copy);
    hiz_debug_pending_[backend.slot_] = lw[0] * lh[0];
    hiz_debug_rects_[backend.slot_].clear();
    for (uint32_t i = 0; i < count && i < 64; ++i) {
      hiz_debug_rects_[backend.slot_].push_back({hiz_records_[i].rect[0], hiz_records_[i].rect[1], hiz_records_[i].rect[2],
                                         hiz_records_[i].rect[3], hiz_records_[i].z});
    }
  }
  // The compute layout's push constants may disturb the graphics ones.
  backend.targets.pushed_constants_[0] = backend.targets.pushed_constants_[1] = backend.targets.pushed_constants_[2] = UINT64_MAX;
  hiz_pred_used_[backend.slot_] += count;
  *base_out = base;
  ++backend.stats_.hiz_windows_tested;
  backend.stats_.hiz_records += count;
  // Back to a depth buffer the next draw can write.
  backend.targets.Transition(target.texture.get(), target.layout, RenderTextureLayout::DEPTH_WRITE);
  return true;
}

// Records a window with the Hi-Z test. False leaves it to the plain loop.
//
// A test costs a pass break, a copy of the depth and a few dispatches, so it is
// only paid for where it can win: a window holding draws that were hidden
// before (their answers are needed, in this frame, to run them or not), and now
// and then a few windows that hold nothing back, to find out which draws are
// hidden. When what the tests find drops under a few percent the second kind
// becomes rare.
bool HiZ::HizFlushWindow(size_t n) {
  if (backend.report.hiz_mode_ < 3 || !hiz_cond_) return false;
  Target* depth = nullptr;
  uint32_t testable = 0, remembered = 0;
  for (size_t i = 0; i < n; ++i) {
    const DrawPacket& pk = backend.recorder.flushing_[i];
    if (!pk.hiz_depth || !pk.hiz_depth->texture || pk.hiz_depth->texture.get() != pk.hiz_texture) continue;
    if (!depth) depth = pk.hiz_depth;
    if (pk.hiz_depth != depth) continue;
    ++testable;
    auto found = hiz_history_.find(pk.hiz_key);
    if (hiz_force_ || (found != hiz_history_.end() && found->second.hidden && backend.swaps_ - found->second.frame <= 90)) {
      ++remembered;
    }
  }
  ++backend.stats_.hiz_windows_all;
  if (!depth || !testable) return false;
  const bool learning = testable >= 3 && hiz_learn_left_ > 0 && backend.swaps_ % hiz_learn_period_ < 2;
  if (!remembered && !learning) {
    ++backend.stats_.hiz_small_windows;
    backend.stats_.hiz_small_draws += testable;
    return false;
  }
  if (hiz_tests_frame_ >= (remembered ? 12u : 3u) + (remembered ? 0u : hiz_tests_remembered_) ||
      !HizEnsureResources()) {
    return false;
  }
  ++hiz_tests_frame_;
  if (remembered) ++hiz_tests_remembered_;
  else --hiz_learn_left_;
  auto usable = [&](const DrawPacket& pk) {
    return pk.hiz_depth == depth && pk.hiz_texture == depth->texture.get();
  };
  // Recorded now, in the grouped order: whatever is not known to be hidden.
  hiz_records_.clear();
  hiz_held_.clear();
  for (size_t i = 0; i < n; ++i) {
    const uint32_t index = backend.recorder.defer_order_[i];
    const DrawPacket& pk = backend.recorder.flushing_[index];
    if (usable(pk)) {
      auto found = hiz_history_.find(pk.hiz_key);
      if (hiz_force_ || (found != hiz_history_.end() && found->second.hidden && backend.swaps_ - found->second.frame <= 90)) {
        hiz_held_.push_back(index);
        continue;
      }
      hiz_records_.push_back({pk.hiz_key, {pk.hiz_rect[0], pk.hiz_rect[1], pk.hiz_rect[2], pk.hiz_rect[3]},
                              pk.hiz_z, false, pk.vs_hash, pk.ps_hash});
    }
    backend.recorder.EmitDraw(pk, backend.draw.flushing_slots_.data() + pk.first_slot, pk.slot_count);
  }
  const uint32_t emitted_records = uint32_t(hiz_records_.size());
  for (uint32_t index : hiz_held_) {
    const DrawPacket& pk = backend.recorder.flushing_[index];
    hiz_records_.push_back({pk.hiz_key, {pk.hiz_rect[0], pk.hiz_rect[1], pk.hiz_rect[2], pk.hiz_rect[3]},
                            pk.hiz_z, true, pk.vs_hash, pk.ps_hash});
  }
  backend.stats_.hiz_held += hiz_held_.size();
  uint32_t base = 0;
  const bool tested = HizRunTest(*depth, uint32_t(hiz_records_.size()), &base);
  // Debug (RENUT_HIZ_VERIFY): each held draw runs whatever the answer, inside an
  // occlusion query, and a draw answered "hidden" that put samples out is a
  // violation.
  const bool verify = tested && hiz_verify_ && hiz_query_pool_ != VK_NULL_HANDLE &&
                      hiz_queries_used_[backend.slot_] + hiz_held_.size() <= kHizMaxQueries;
  uint32_t first_query = 0;
  if (verify && !hiz_held_.empty()) {
    first_query = backend.slot_ * kHizMaxQueries + hiz_queries_used_[backend.slot_];
    vkCmdResetQueryPool(static_cast<VulkanCommandList*>(backend.recorder.L())->vk, hiz_query_pool_, first_query,
                        uint32_t(hiz_held_.size()));
    hiz_queries_used_[backend.slot_] += uint32_t(hiz_held_.size());
  }
  if (tested) {
    for (size_t r = 0; r < hiz_records_.size(); ++r) {
      const bool held = hiz_records_[r].held;
      const uint32_t query = verify && held ? first_query + uint32_t(r - emitted_records) : UINT32_MAX;
      hiz_pending_[backend.slot_].push_back({hiz_records_[r].key, held, query, hiz_records_[r].vs, hiz_records_[r].ps});
    }
  } else {
    ++backend.stats_.hiz_test_failed;
  }
  for (size_t k = 0; k < hiz_held_.size(); ++k) {
    DrawPacket pk = backend.recorder.flushing_[hiz_held_[k]];
    if (tested && !verify) pk.hiz_pred_offset = (base + emitted_records + uint32_t(k)) * 4;
    if (verify) pk.hiz_query = first_query + uint32_t(k);
    backend.recorder.EmitDraw(pk, backend.draw.flushing_slots_.data() + pk.first_slot, pk.slot_count);
  }
  return true;
}

// The answers of a slot's last frame, once its fence has passed: what each key
// was found to be, for the frames that follow.
void HiZ::HizReadResults(uint32_t slot) {
  std::vector<HizPending>& pending = hiz_pending_[slot];
  if (hiz_debug_pending_[slot] && hiz_read_[slot]) {
    const auto* all = static_cast<const float*>(hiz_read_[slot]->map());
    if (all) {
      const float* tiles = all + 2 * kHizMaxRecords;
      const uint32_t n = hiz_debug_pending_[slot];
      uint32_t far_tiles = 0;
      float lowest = 1.f, highest = 0.f;
      for (uint32_t i = 0; i < n; ++i) {
        far_tiles += tiles[i] >= 0.9999f;
        lowest = std::min(lowest, tiles[i]);
        highest = std::max(highest, tiles[i]);
      }
      std::string line = fmt::format("Renut: Plume Hi-Z debug: level 0 has {} tiles, {} at the far plane, range {}..{};",
                                     n, far_tiles, lowest, highest);
      for (const auto& r : hiz_debug_rects_[slot]) {
        if (&r - hiz_debug_rects_[slot].data() >= 6) break;
        line += fmt::format(" rect({:.0f},{:.0f})-({:.0f},{:.0f}) z={:.4f}", r[0], r[1], r[2], r[3], r[4]);
      }
      REXLOG_INFO("{}", line);
      hiz_read_[slot]->unmap();
    }
    hiz_debug_pending_[slot] = 0;
  }
  if (pending.empty()) return;
  if (hiz_read_[slot]) {
    const auto* answers = static_cast<const uint32_t*>(hiz_read_[slot]->map());
    if (answers) {
      // One verdict a key per frame: hidden only if every instance was.
      std::unordered_map<uint64_t, bool> hidden;
      std::vector<uint64_t> samples;
      if (hiz_verify_ && hiz_query_pool_ != VK_NULL_HANDLE && hiz_queries_used_[slot]) {
        samples.assign(hiz_queries_used_[slot], 0);
        vkGetQueryPoolResults(static_cast<VulkanDevice*>(backend.device_)->vk, hiz_query_pool_, slot * kHizMaxQueries,
                              hiz_queries_used_[slot], samples.size() * 8, samples.data(), 8,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
      }
      for (size_t i = 0; i < pending.size(); ++i) {
        const bool occluded = answers[i] == 0;
        if (pending[i].query != UINT32_MAX && !samples.empty()) {
          const uint32_t q = pending[i].query - slot * kHizMaxQueries;
          if (q < samples.size()) {
            ++backend.stats_.hiz_verified;
            if (occluded && samples[q]) ++backend.stats_.hiz_violations;
            if (!occluded && !samples[q]) {
              ++backend.stats_.hiz_missed_hidden;
              // Why the test could not tell: no depth there yet, or some.
              const float m = reinterpret_cast<const float*>(answers)[kHizMaxRecords + i];
              if (m >= 0.9999f) ++backend.stats_.hiz_missed_empty;
              auto& pair = backend.report.hiz_zero_pairs_[pending[i].vs * 0x9E3779B97F4A7C15ull ^ pending[i].ps];
              pair = {pair[0] + 1, pending[i].vs, pending[i].ps};
            }
          }
        }
        auto [it, fresh] = hidden.try_emplace(pending[i].key, occluded);
        if (!fresh) it->second = it->second && occluded;
        ++backend.stats_.hiz_answered;
        ++hiz_total_answered_;
        if (occluded) {
          ++backend.stats_.hiz_occluded;
          ++hiz_total_hidden_;
          if (pending[i].held) ++backend.stats_.hiz_held_skipped;
        } else if (pending[i].held) {
          ++backend.stats_.hiz_held_visible;
        }
      }
      for (const auto& [key, occluded] : hidden) {
        HizHistory& h = hiz_history_[key];
        h.hidden = occluded;
        h.frame = backend.swaps_;
      }
      hiz_read_[slot]->unmap();
    }
  }
  pending.clear();
  hiz_pred_used_[slot] = 0;
  hiz_queries_used_[slot] = 0;
  // What the tests find decides how often to look for more.
  if (hiz_total_answered_ - hiz_gain_answered_ >= 600) {
    const uint64_t answered = hiz_total_answered_ - hiz_gain_answered_;
    const uint64_t hidden = hiz_total_hidden_ - hiz_gain_hidden_;
    // Nothing (under 2%) found hidden: look every 600 frames instead of 8.
    hiz_learn_period_ = hidden * 50 < answered ? 600 : 8;
    hiz_gain_answered_ = hiz_total_answered_;
    hiz_gain_hidden_ = hiz_total_hidden_;
  }
  // Keys that stopped appearing.
  if (hiz_history_.size() > 20000) {
    for (auto it = hiz_history_.begin(); it != hiz_history_.end();) {
      it = backend.swaps_ - it->second.frame > 600 ? hiz_history_.erase(it) : std::next(it);
    }
  }
}

}  // namespace renut::gpu
