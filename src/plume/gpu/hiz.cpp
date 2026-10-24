// Hi-Z culling.
#include "internal.h"

namespace renut::gpu {

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
  const RegisterFile& regs = *backend.register_file_;
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
  if (!ShaderInterpreter::CanInterpretShader(vs)) return false;
  struct Sink final : ShaderInterpreter::ExportSink {
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
  auto interpreter = std::make_unique<ShaderInterpreter>(*backend.register_file_, *backend.memory_);
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
  ++backend.stats_.hiz_seen[kind];
  if (!index_count) return ++backend.stats_.hiz_why[0], false;
  switch (prim_type) {
    case xenos::PrimitiveType::kTriangleList:
    case xenos::PrimitiveType::kTriangleStrip:
    case xenos::PrimitiveType::kTriangleFan:
    case xenos::PrimitiveType::kQuadList: break;
    default: return ++backend.stats_.hiz_why[1], false;
  }
  const RegisterFile& regs = *backend.register_file_;
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
  // User clip planes (the quarter-resolution mirrored pass clips at plane 0):
  // the shaders keep what has dot(clip position, plane) >= 0 on every enabled
  // plane, so a box wholly below one of them draws nothing.
  if (const uint32_t ucp = clip.clip_disable ? 0u : uint32_t(clip.ucp_ena)) {
    for (uint32_t i = 0; i < 6; ++i) {
      if (!(ucp & (1u << i))) continue;
      const uint32_t r = XE_GPU_REG_PA_CL_UCP_0_X + i * 4;
      if (wholly_outside(regs.Get<float>(r), regs.Get<float>(r + 1), regs.Get<float>(r + 2), regs.Get<float>(r + 3))) {
        ++backend.stats_.hiz_frustum[kind];
        ++backend.stats_.hiz_clip_plane[kind];
        if (cold_slot) cold_slot->culled += sampled_cold ? 8 : 1;
        return backend.report.hiz_mode_ >= 2;
      }
    }
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
  return false;
}

}  // namespace renut::gpu
