// Pipeline workers, the recipe file and pipeline prediction.
#include "internal.h"

namespace renut::gpu {

uint64_t Pipelines::ProgramKey(const RenderGraphicsPipelineDesc& desc) {
  KeyBytes bytes;
  bytes.Put(desc.vertexShader);
  bytes.Put(desc.geometryShader);
  bytes.Put(desc.pixelShader);
  for (uint32_t i = 0; i < desc.specConstantsCount; ++i) {
    bytes.Put(desc.specConstants[i].index);
    bytes.Put(desc.specConstants[i].value);
  }
  return bytes.Hash();
}

void Pipelines::StartPipelineWorker() {
  if (!pipeline_workers_.empty()) return;
  stop_pipeline_worker_ = false;
  lookup_on_workers_ = PlumePipelineLookupSupported();
  // Compiling is slow on one core and free on others: a few workers, kept off
  // the cores' best share so the guest and command list threads win contention.
  const uint32_t cores = std::max(1u, std::thread::hardware_concurrency());
  const uint32_t count = std::clamp(cores / 3, 1u, 3u);
  // One more on a device with cores to spare, for work whose draw is missing
  // from the screen only: it sleeps whenever nothing is.
  const uint32_t urgent_only = cores >= 8 ? 1 : 0;
  for (uint32_t i = 0; i < count + urgent_only; ++i) {
    pipeline_workers_.emplace_back([this, i, urgent = i >= count] {
      pthread_setname_np(pthread_self(), fmt::format("PlumePipeline{}", i).c_str());
      int nice = INT32_MIN;
      for (;;) {
        std::shared_ptr<PipelineJob> job;
        {
          std::unique_lock lock(pipeline_mutex_);
          pipeline_ready_.wait(lock, [this, urgent] {
            return stop_pipeline_worker_ || !pipeline_lookup_queue_.empty() || !pipeline_urgent_queue_.empty() ||
                   (!urgent && !pipeline_queue_.empty());
          });
          if (stop_pipeline_worker_) return;
          // Lookups and the rest oldest first, so a steady stream of new
          // pipelines cannot starve one that was queued earlier; urgent ones
          // by how much their draw is missed.
          if (!pipeline_lookup_queue_.empty()) {
            job = std::move(pipeline_lookup_queue_.front());
            pipeline_lookup_queue_.pop_front();
          } else if (!pipeline_urgent_queue_.empty()) {
            job = TakeUrgentJob();
          } else {
            job = std::move(pipeline_queue_.front());
            pipeline_queue_.pop_front();
          }
        }
        // A job whose draw is missing from the screen competes for the CPU; one
        // that only replaces a stand-in yields to the game further.
        const int wanted = job->urgent ? 4 : 10;
        if (wanted != nice) {
          renutSetThreadNice(wanted);
          nice = wanted;
        }
        job->desc.inputElements = job->elements.data();
        job->desc.inputSlots = job->slots.data();
        job->desc.specConstants = job->constants.data();
        const auto start = std::chrono::steady_clock::now();
        auto finish = [&] {
          job->duration_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - start).count());
          job->done.store(true, std::memory_order_release);
        };
        // First the cache: a sibling of the same program may have compiled it
        // since the job was queued. A miss joins the compiles, behind the
        // lookups still waiting.
        if (lookup_on_workers_ && !job->looked_up) {
          {
            PlumePipelineLookupOnly only;
            try { job->result = CreateGraphicsPipeline(backend.device_, job->desc); } catch (const std::exception&) {}
          }
          job->looked_up = true;
          if (job->result) {
            job->found = true;
            finish();
          } else {
            {
              std::lock_guard lock(pipeline_mutex_);
              (job->urgent ? pipeline_urgent_queue_ : pipeline_queue_).push_back(std::move(job));
            }
            pipeline_ready_.notify_all();
          }
          continue;
        }
        // Then one compile per program: a job whose program another worker is
        // compiling waits for that compile and looks it up afterwards.
        {
          std::lock_guard lock(pipeline_mutex_);
          auto [waiting, first] = compiling_programs_.try_emplace(job->program);
          if (!first) {
            job->waited = true;
            waiting->second.push_back(std::move(job));
            continue;
          }
        }
        try { job->result = CreateGraphicsPipeline(backend.device_, job->desc); }
        catch (const std::exception& e) { REXLOG_WARN("Renut: Plume async pipeline failed: {}", e.what()); }
        const uint64_t program = job->program;
        finish();
        bool requeued = false;
        {
          std::lock_guard lock(pipeline_mutex_);
          auto waiting = compiling_programs_.find(program);
          if (waiting != compiling_programs_.end()) {
            for (auto& sibling : waiting->second) {
              sibling->looked_up = false;
              (lookup_on_workers_ ? pipeline_lookup_queue_
               : sibling->urgent  ? pipeline_urgent_queue_
                                  : pipeline_queue_).push_front(std::move(sibling));
              requeued = true;
            }
            compiling_programs_.erase(waiting);
          }
        }
        if (requeued) pipeline_ready_.notify_all();
      }
    });
  }
}

void Pipelines::StopPipelineWorker() {
  if (pipeline_workers_.empty()) return;
  {
    std::lock_guard lock(pipeline_mutex_);
    stop_pipeline_worker_ = true;
    pipeline_lookup_queue_.clear();
    pipeline_urgent_queue_.clear();
    pipeline_queue_.clear();
    compiling_programs_.clear();
  }
  pipeline_ready_.notify_all();
  for (std::thread& worker : pipeline_workers_) worker.join();
  pipeline_workers_.clear();
  backend.draw.pending_pipelines_.clear();
  pending_programs_.clear();
}

void Pipelines::CountJob(const PipelineJob& job) {
  backend.stats_.pipeline_us += job.duration_us;
  ++backend.stats_.pipelines;
  if (job.found) {
    ++backend.stats_.pipelines_found_ahead;
  } else {
    ++backend.stats_.pipelines_compiled;
    backend.stats_.compile_us += job.duration_us;
  }
  if (job.waited) ++backend.stats_.pipelines_waited;
  auto program = pending_programs_.find(job.program);
  if (program != pending_programs_.end() && --program->second == 0) pending_programs_.erase(program);
}

// A finished job used to wait for its own draw to come back and claim it. One
// that never did (a scene change, a one-off effect) kept its slot forever, and
// once 128 of those piled up every new pipeline compiled on the command list
// thread again. Called once a frame, this installs them all.
void Pipelines::ReapPipelines() {
  for (auto it = backend.draw.pending_pipelines_.begin(); it != backend.draw.pending_pipelines_.end();) {
    PipelineJob& job = *it->second;
    if (!job.done.load(std::memory_order_acquire)) {
      ++it;
      continue;
    }
    CountJob(job);
    if (job.result) {
      std::unique_ptr<RenderPipeline>& slot = pipelines_[it->first];
      slot = std::move(job.result);
      backend.draw.standins_[job.family] = slot.get();
    } else {
      backend.draw.failed_pipelines_.insert(it->first);
      REXLOG_WARN("Renut: Plume pipeline creation failed (vs {:016X}, ps {:016X})", job.vs_hash, job.ps_hash);
    }
    it = backend.draw.pending_pipelines_.erase(it);
  }
}

void Pipelines::StartRecipes() {
  recipes_ = REXCVAR_GET(renut_plume_precompile) && !g_miss_dump_directory.empty();
  if (!recipes_) return;
  const std::string files = g_miss_dump_directory.substr(0, g_miss_dump_directory.rfind('/'));
  const std::string path = files + "/plume-recipes.bin";
  auto read_file = [](const std::string& name) {
    std::vector<uint8_t> bytes;
    if (FILE* file = std::fopen(name.c_str(), "rb")) {
      std::fseek(file, 0, SEEK_END);
      const long size = std::ftell(file);
      std::fseek(file, 0, SEEK_SET);
      if (size > 0) {
        bytes.resize(size_t(size));
        if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) bytes.clear();
      }
      std::fclose(file);
    }
    return bytes;
  };
  // Records are a 32-bit size and a recipe's bytes, or a scene tag (the size
  // with kRecipeTagBit). One cut short by the app being killed mid-write ends
  // the list and is cut off the file.
  auto recipes = std::make_shared<std::vector<std::pair<uint64_t, Recipe>>>();
  std::vector<std::pair<uint64_t, uint32_t>> tags;
  RecipeHeader header;
  // Returns where the good records end, 0 for a file that is not a list.
  auto parse = [&](const std::vector<uint8_t>& bytes) -> size_t {
    if (bytes.size() < sizeof(header) || std::memcmp(bytes.data(), &header, sizeof(header)) != 0) return 0;
    size_t good_end = sizeof(header);
    while (bytes.size() - good_end >= sizeof(uint32_t)) {
      uint32_t size = 0;
      std::memcpy(&size, bytes.data() + good_end, sizeof(size));
      const uint8_t* at = bytes.data() + good_end + sizeof(size);
      const uint32_t length = size & ~kRecipeTagBit;
      if (size_t(bytes.data() + bytes.size() - at) < length) break;
      if (size & kRecipeTagBit) {
        if (length != kRecipeTagSize) break;
        uint64_t recipe = 0;
        uint32_t scene = 0;
        std::memcpy(&recipe, at, sizeof(recipe));
        std::memcpy(&scene, at + sizeof(recipe), sizeof(scene));
        if (scene_tags_.insert(SceneTagKey(recipe, scene)).second) tags.emplace_back(recipe, scene);
      } else {
        Recipe recipe;
        RecipeReader reader{at, at + length};
        const uint64_t key = XXH3_64bits(at, length);
        if (VisitRecipe(reader, recipe) && recorded_recipes_.insert(key).second) {
          recipes->emplace_back(key, std::move(recipe));
        }
      }
      good_end += sizeof(size) + length;
    }
    return good_end;
  };
  // The list shipped with the app (the app copies it out of the APK) comes
  // first: it stands in for the pipelines UnleashedRecomp predicts from the
  // assets a scene loads, so even the first start holds each loading screen
  // for its scene's pipelines instead of compiling them in play. The player's
  // own file only adds what the shipped list lacks.
  const size_t seeded = parse(read_file(files + "/plume-recipes.seed")) ? recipes->size() : 0;
  const std::vector<uint8_t> bytes = read_file(path);
  const size_t good_end = parse(bytes);
  const bool valid = good_end != 0;
  if (seeded) REXLOG_INFO("Renut: Plume pipeline recipes: {} shipped, {} more recorded here", seeded,
                          recipes->size() - seeded);
  if (!valid) {
    if ((recipe_file_ = std::fopen(path.c_str(), "wb"))) {
      std::fwrite(&header, sizeof(header), 1, recipe_file_);
      std::fflush(recipe_file_);
    }
  } else {
    if (good_end != bytes.size()) truncate(path.c_str(), off_t(good_end));
    recipe_file_ = std::fopen(path.c_str(), "ab");
  }
  if (!recipe_file_) REXLOG_WARN("Renut: Plume pipeline recipes cannot be written to {}", path);
  if (backend.low_memory_) {
    REXLOG_INFO("Renut: Plume pipeline recipes: {} in {}; not built ahead in low-memory mode", recipes->size(), path);
    return;
  }

  // One group per program, built by one thread: its first recipe compiles
  // and the others find the program in the cache.
  PrecompileState& p = g_precompile;
  {
    std::vector<std::vector<std::pair<uint64_t, Recipe>>> grouped;
    std::vector<uint64_t> program_of_group;
    std::unordered_map<uint64_t, uint32_t> group_of, group_of_recipe;
    for (auto& item : *recipes) {
      const uint64_t program = RecipeProgram(item.second);
      auto [it, added] = group_of.try_emplace(program, uint32_t(grouped.size()));
      if (added) {
        grouped.emplace_back();
        program_of_group.push_back(program);
      }
      group_of_recipe.emplace(item.first, it->second);
      grouped[it->second].push_back(std::move(item));
    }
    std::lock_guard lock(p.mutex);
    p.recipes.clear();
    p.groups.clear();
    p.warm_only.clear();
    p.state.clear();
    p.group_size.clear();
    p.programs.clear();
    p.urgent.clear();
    p.background.clear();
    p.busy = 0;
    p.scene_groups.clear();
    p.done = 0;
    p.total = 0;
    p.running = false;
    for (size_t g = 0; g < grouped.size(); ++g) {
      p.AddGroup(std::move(grouped[g]), false);
      p.programs.insert(program_of_group[g]);
    }
    for (const auto& [recipe, scene] : tags) {
      auto group = group_of_recipe.find(recipe);
      if (group == group_of_recipe.end()) continue;
      std::vector<uint32_t>& list = p.scene_groups[scene];
      if (std::find(list.begin(), list.end(), group->second) == list.end()) list.push_back(group->second);
    }
    // The boot's own first; then the rest in the order they were first drawn.
    if (auto boot = p.scene_groups.find(0); boot != p.scene_groups.end()) {
      p.urgent.assign(boot->second.begin(), boot->second.end());
    }
  }
  stop_precompile_ = false;
  const uint32_t cores = std::max(1u, std::thread::hardware_concurrency());
  const uint32_t count = std::clamp(cores * 2 / 3, 2u, 6u);
  REXLOG_INFO("Renut: Plume knows {} pipelines ({} programs, {} tags over {} scenes) from {}; each scene's are "
              "built while it loads, on {} threads",
              recipes->size(), p.groups.size(), tags.size(), p.scene_groups.size(), path, count);
  recipes->clear();
  p.predicting = true;
  // The threads stay for the whole run: predicted programs (PredictPipelines)
  // arrive as models load.
  for (uint32_t t = 0; t < count; ++t) {
    precompile_threads_.emplace_back([this, t] {
      pthread_setname_np(pthread_self(), fmt::format("PlumePrebuild{}", t).c_str());
      PrecompileState& p = g_precompile;
      int nice = INT32_MIN;
      for (;;) {
        uint32_t g = UINT32_MAX;
        std::vector<std::pair<uint64_t, Recipe>> work;
        bool warm = false;
        {
          std::unique_lock lock(p.mutex);
          for (;;) {
            if (stop_precompile_.load(std::memory_order_relaxed)) return;
            g = p.TakeGroup(t == 0);
            if (g != UINT32_MAX) break;
            if (p.busy == 0 && p.running && p.urgent.empty() && p.background.empty()) {
              p.running = false;
              REXLOG_INFO("Renut: Plume built {} of {} pipelines ahead in {} ms", p.done.load() - p.batch_failed,
                          p.total.load(),
                          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                p.batch_start)
                              .count());
              p.changed.notify_all();
            }
            p.changed.wait(lock);
          }
          ++p.busy;
          for (const uint32_t index : p.groups[g]) work.push_back(p.recipes[index]);
          warm = p.warm_only[g];
        }
        // At full speed while a loading screen is up; during play only this
        // thread works, at the lowest priority.
        const int wanted = p.loading.load(std::memory_order_relaxed) ? 0 : 19;
        if (wanted != nice) {
          renutSetThreadNice(wanted);
          nice = wanted;
        }
        const Recipe& first = work[0].second;
        CompiledShader vs, ps;
        const ShaderCacheEntry* vs_entry = store_->FindContainer(first.vs);
        const ShaderCacheEntry* ps_entry = first.ps ? store_->FindContainer(first.ps) : nullptr;
        bool ok = vs_entry && ShaderStore::BuildModules(backend.device_, vs_entry, !first.domain, true, vs) &&
                  (!first.ps || (ps_entry && ShaderStore::BuildModules(backend.device_, ps_entry, false, false, ps)));
        const RenderShader* vs_module = !ok                                   ? nullptr
                                        : first.form == kRecipeFetchSpec      ? vs.fetch_spec_module.get()
                                        : first.form == kRecipeDepth          ? vs.depth_module.get()
                                        : first.form == kRecipeDepthFetchSpec ? vs.depth_fetch_spec_module.get()
                                                                              : vs.module.get();
        uint32_t failed = 0;
        for (auto& [key, recipe] : work) {
          std::unique_ptr<RenderPipeline> pipeline;
          if (vs_module && !stop_precompile_.load(std::memory_order_relaxed)) {
            RenderGraphicsPipelineDesc desc = recipe.desc;
            desc.pipelineLayout = backend.layout_.get();
            desc.vertexShader = vs_module;
            desc.geometryShader = recipe.gs == 1 ? backend.draw.rect_list_gs_.get() : recipe.gs == 2 ? backend.draw.point_list_gs_.get() : nullptr;
            desc.pixelShader = recipe.ps ? ps.module.get() : nullptr;
            desc.inputElements = recipe.elements.data();
            desc.inputElementsCount = uint32_t(recipe.elements.size());
            desc.inputSlots = recipe.slots.data();
            desc.inputSlotsCount = uint32_t(recipe.slots.size());
            desc.specConstants = recipe.constants.data();
            desc.specConstantsCount = uint32_t(recipe.constants.size());
            try { pipeline = CreateGraphicsPipeline(backend.device_, desc); } catch (const std::exception&) {}
          }
          if (!pipeline) {
            ++failed;
          } else if (!warm) {
            std::lock_guard lock(prebuilt_mutex_);
            prebuilt_.emplace(key, std::move(pipeline));
          }
        }
        {
          std::lock_guard lock(p.mutex);
          p.state[g] = PrecompileState::kDone;
          --p.busy;
          p.batch_failed += failed;
        }
        p.done.fetch_add(uint32_t(work.size()), std::memory_order_relaxed);
        p.changed.notify_all();
      }
    });
  }
}

void Pipelines::StopRecipes() {
  stop_precompile_ = true;
  g_precompile.predicting = false;
  {
    std::lock_guard lock(g_predict.mutex);
    g_predict.pairs.clear();
    g_predict.queued = 0;
  }
  {
    std::lock_guard lock(g_precompile.mutex);  // a thread between its check and its wait
  }
  g_precompile.changed.notify_all();
  for (std::thread& thread : precompile_threads_) thread.join();
  precompile_threads_.clear();
  {
    std::lock_guard lock(g_precompile.mutex);
    g_precompile.running = false;
  }
  g_precompile.changed.notify_all();
  {
    std::lock_guard lock(prebuilt_mutex_);
    prebuilt_.clear();
  }
  recorded_recipes_.clear();
  scene_tags_.clear();
  recipe_of_.clear();
  if (recipe_file_) std::fclose(recipe_file_);
  recipe_file_ = nullptr;
}

// Pipeline prediction (PlumePredictPipelines): the shader pairs of the models
// that loaded, each turned into the recipe a draw of it would make - the same
// vertex inputs and specialization constants IssueDraw works out - with a fixed
// state borrowed from a real draw. The driver's cache is keyed by the program
// (shaders and specialization), not that state, so the pipeline the draw makes
// later finds its program built. Runs on the command thread, a few
// milliseconds at a time.
void Pipelines::PredictPipelines() {
  std::vector<PlumeShaderPair> pairs;
  {
    std::lock_guard lock(g_predict.mutex);
    pairs.swap(g_predict.pairs);
  }
  if (pairs.empty()) return;
  PrecompileState& p = g_precompile;
  const auto start = std::chrono::steady_clock::now();
  uint32_t added = 0, known = 0, unknown = 0;
  size_t i = 0;
  for (; i < pairs.size(); ++i) {
    if (i && std::chrono::steady_clock::now() - start > std::chrono::milliseconds(8)) break;
    const PlumeShaderPair& pair = pairs[i];
    const int form_kind = pair.ps_dwords ? 0 : 1;
    if (!predict_desc_set_[form_kind]) {
      if (!predict_desc_set_[0]) break;  // no draw yet: later
      predict_desc_[1] = predict_desc_[0];
      predict_desc_[1].renderTargetCount = 0;
      predict_desc_set_[1] = true;
    }
    auto* vs = static_cast<HostShader*>(backend.shaders.LoadShaderByContents(
        xenos::ShaderType::kVertex, backend.memory_->TranslatePhysical<const uint32_t*>(pair.vs_address), pair.vs_dwords));
    HostShader* ps = nullptr;
    if (pair.ps_dwords) {
      ps = static_cast<HostShader*>(backend.shaders.LoadShaderByContents(
          xenos::ShaderType::kPixel, backend.memory_->TranslatePhysical<const uint32_t*>(pair.ps_address), pair.ps_dwords));
    }
    if (!vs || !vs->entry || (pair.ps_dwords && (!ps || !ps->entry))) {
      ++unknown;
      continue;
    }
    if (!vs->compiled) vs->compiled = store_->Build(backend.device_, vs->entry, true, true);
    if (ps && !ps->compiled) ps->compiled = store_->Build(backend.device_, ps->entry, false);
    if (!vs->compiled || (ps && !ps->compiled)) {
      ++unknown;
      continue;
    }
    if (!vs->elements_decoded) DecodeLiveElements(*vs);

    Recipe recipe;
    recipe.vs = vs->entry->hash;
    recipe.ps = ps ? ps->entry->hash : 0;
    recipe.desc = predict_desc_[form_kind];
    // Vertex inputs, as IssueDraw lays them out (every stream taken as bound).
    std::array<bool, 17> slot_used{};
    uint32_t input_remap[RENUT_INPUT_REMAP_COUNT] = {};
    uint32_t packed_inputs = 0, int_inputs = 0;
    bool r11g11b10_normals = false, zero_used = false;
    for (auto [location, integer] : vs->compiled->inputs) {
      const LiveElement* element = nullptr;
      for (const LiveElement& e : vs->elements) {
        if (e.location == location) {
          element = &e;
          break;
        }
      }
      InputDecode decode = InputDecode::kNone;
      RenderFormat format = element && !integer
                                ? VertexFormat(element->format, element->is_signed, element->normalized, decode)
                                : RenderFormat::UNKNOWN;
      if (!element || format == RenderFormat::UNKNOWN || !element->stride || element->slot >= slot_used.size() ||
          (slot_used[element->slot] &&
           std::find_if(recipe.slots.begin(), recipe.slots.end(), [&](const RenderInputSlot& s) {
             return s.index == element->slot && s.stride != element->stride;
           }) != recipe.slots.end())) {
        recipe.elements.emplace_back("TEXCOORD", location, location,
                                     integer ? RenderFormat::R32G32B32A32_UINT : RenderFormat::R32G32B32A32_FLOAT,
                                     kZeroBinding, 0);
        zero_used = true;
        continue;
      }
      recipe.elements.emplace_back("TEXCOORD", location, location, format, element->slot, element->offset);
      switch (decode) {
        case InputDecode::kInteger: int_inputs |= 1u << location; break;
        case InputDecode::kPacked1010102: packed_inputs |= 1u << location; break;
        case InputDecode::kPacked111110:
          r11g11b10_normals |= element->usage == 3 || element->usage == 6 || element->usage == 7;
          break;
        default: break;
      }
      if (!slot_used[element->slot]) {
        slot_used[element->slot] = true;
        recipe.slots.emplace_back(element->slot, element->stride);
      }
      if (location < RENUT_INPUT_REMAP_COUNT) input_remap[location] = element->remap;
    }
    if (zero_used) recipe.slots.emplace_back(kZeroBinding, 0);
    // Specialization constants in IssueDraw's order: alpha test as the pass
    // draws it, no user clip planes, the boolean registers at 0.
    recipe.constants.emplace_back(0, (pair.alpha_test && ps ? SPEC_CONSTANT_ALPHA_TEST : 0u) |
                                         (r11g11b10_normals ? SPEC_CONSTANT_R11G11B10_NORMAL : 0u));
    if (packed_inputs) recipe.constants.emplace_back(1, packed_inputs);
    if (int_inputs) recipe.constants.emplace_back(2, int_inputs);
    for (uint32_t l = 0; l < RENUT_SPEC_INPUT_REMAP_LOCATIONS; ++l) {
      if (input_remap[l]) recipe.constants.emplace_back(RENUT_SPEC_INPUT_REMAP_BASE + l, input_remap[l]);
    }
    const bool fetch_spec = backend.swap.fetch_spec_mode_ && vs->compiled->fetch_spec_module;
    if (fetch_spec) {
      for (const auto& [slot, entry] : vs->live_fetches) {
        for (uint32_t word = 0; word < 4; ++word) {
          if (entry[word]) recipe.constants.emplace_back(kFetchSpecBase + slot * 4 + word, entry[word]);
        }
      }
    }
    if (backend.draw.rel_constants_mode_ && backend.draw.rel_constants_mode_ < 10 &&
        (vs->entry->specConstantsMask & RENUT_SHADER_FLAG_REL_CONSTANTS)) {
      recipe.constants.emplace_back(RENUT_SPEC_REL_CONSTANTS, backend.draw.rel_constants_mode_);
    }
    const bool depth = backend.swap.depth_strip_mode_ && !ps &&
                       (fetch_spec ? vs->compiled->depth_fetch_spec_module : vs->compiled->depth_module);
    recipe.form = depth ? (fetch_spec ? kRecipeDepthFetchSpec : kRecipeDepth)
                        : (fetch_spec ? kRecipeFetchSpec : kRecipeModule);
    std::string bytes;
    RecipeWriter writer{bytes};
    VisitRecipe(writer, recipe);
    const uint64_t key = XXH3_64bits(bytes.data(), bytes.size());
    const uint64_t program = RecipeProgram(recipe);
    std::lock_guard lock(p.mutex);
    if (!p.programs.insert(program).second) {
      ++known;
      continue;
    }
    std::vector<std::pair<uint64_t, Recipe>> group;
    group.emplace_back(key, std::move(recipe));
    const uint32_t g = p.AddGroup(std::move(group), true);
    // While a scene loads its screen waits for these too, and they go first;
    // during play they are left to the one thread that works then.
    if (!p.loading) {
      p.background.push_back(g);
    } else {
      p.urgent.push_front(g);
      p.scene_missing.push_back(g);
      ++p.scene_total;
      ++p.scene_left;
    }
    ++added;
  }
  const size_t done = i;
  if (done < pairs.size()) {
    std::lock_guard lock(g_predict.mutex);
    g_predict.pairs.insert(g_predict.pairs.begin(), pairs.begin() + done, pairs.end());
  }
  g_predict.queued.fetch_sub(uint32_t(done), std::memory_order_relaxed);
  if (added) p.changed.notify_all();
  predict_added_ += added;
  predict_known_ += known;
  predict_unknown_ += unknown;
  if (done && (added || unknown)) {
    REXLOG_INFO("Renut: Plume predicted {} new programs from {} shader pairs ({} already known, {} without a "
                "recompiled shader); {} new, {} known so far; draws so far used {} programs, {} built ahead",
                added, done, known, unknown, predict_added_, predict_known_, p.draw_programs.load(),
                p.draw_programs_ahead.load());
  }
}

// Flushed at once: a game closed by swiping it away never runs the shutdown
// that would close the file.
void Pipelines::WriteRecord(uint32_t size, const void* data) {
  if (!recipe_file_) return;
  std::fwrite(&size, sizeof(size), 1, recipe_file_);
  std::fwrite(data, 1, size & ~kRecipeTagBit, recipe_file_);
  std::fflush(recipe_file_);
}

std::unique_ptr<RenderPipeline> Pipelines::TakeRecipe(Recipe& recipe, uint64_t key) {
  std::string bytes;
  RecipeWriter writer{bytes};
  VisitRecipe(writer, recipe);
  const uint64_t recipe_key = XXH3_64bits(bytes.data(), bytes.size());
  recipe_of_[key] = recipe_key;
  {
    const uint64_t program = RecipeProgram(recipe);
    std::lock_guard lock(g_precompile.mutex);
    if (drawn_programs_.insert(program).second) {
      ++g_precompile.draw_programs;
      if (g_precompile.programs.count(program)) ++g_precompile.draw_programs_ahead;
    }
  }
  {
    std::lock_guard lock(prebuilt_mutex_);
    auto found = prebuilt_.find(recipe_key);
    if (found != prebuilt_.end()) {
      std::unique_ptr<RenderPipeline> pipeline = std::move(found->second);
      prebuilt_.erase(found);
      return pipeline;
    }
  }
  if (recorded_recipes_.insert(recipe_key).second) WriteRecord(uint32_t(bytes.size()), bytes.data());
  return nullptr;
}

void Pipelines::NoteSceneUse(uint64_t key) {
  const uint32_t scene = g_precompile.scene.load(std::memory_order_relaxed);
  if (scene != tag_scene_) {
    tag_scene_ = scene;
    scene_seen_.reset();
  }
  const size_t bit = size_t(key) & (scene_seen_.size() - 1);
  if (scene_seen_.test(bit)) return;
  scene_seen_.set(bit);
  auto recipe = recipe_of_.find(key);
  if (recipe == recipe_of_.end()) return;
  if (!scene_tags_.insert(SceneTagKey(recipe->second, scene)).second) return;
  uint8_t tag[kRecipeTagSize];
  std::memcpy(tag, &recipe->second, sizeof(uint64_t));
  std::memcpy(tag + sizeof(uint64_t), &scene, sizeof(scene));
  WriteRecord(kRecipeTagBit | kRecipeTagSize, tag);
}

}  // namespace renut::gpu
