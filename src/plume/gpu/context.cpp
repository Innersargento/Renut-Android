// Vulkan context set-up and tear-down and the command processor's worker loop.
#include "internal.h"

namespace renut::gpu {

#include "plume/shaders/point_list.geom.inc"
#include "plume/shaders/rect_list.geom.inc"

bool PlumeCommandProcessor::SetupContext() {
  // Direct3D's doorbell wakes the worker through this (native_d3d.h).
  native_d3d::SetWake([this] { write_ptr_index_event_->Set(); });
  auto* renderer = PlumeRenderer::Get();
  if (!renderer) {
    REXLOG_ERROR("Renut: Plume command processor without a Plume renderer");
    return false;
  }
  device_ = renderer->device();
  queue_ = renderer->queue();
  miss_dump_directory_ = g_miss_dump_directory;
  store_ = std::make_unique<ShaderStore>();

  // Descriptor sets: the recompiled shaders' bindless heaps (register t0 in
  // spaces 0..2 and s0 in space 3 map to set N, binding 0).
  RenderDescriptorRange range_2d(RenderDescriptorRangeType::TEXTURE, 0, kMaxTextures2D);
  RenderDescriptorRange range_3d(RenderDescriptorRangeType::TEXTURE, 0, kMaxTextures3D);
  RenderDescriptorRange range_cube(RenderDescriptorRangeType::TEXTURE, 0, kMaxTexturesCube);
  RenderDescriptorRange range_samplers(RenderDescriptorRangeType::SAMPLER, 0, kMaxSamplers);
  RenderDescriptorSetDesc sets[] = {
      RenderDescriptorSetDesc(&range_2d, 1, true, kMaxTextures2D),
      RenderDescriptorSetDesc(&range_3d, 1, true, kMaxTextures3D),
      RenderDescriptorSetDesc(&range_cube, 1, true, kMaxTexturesCube),
      RenderDescriptorSetDesc(&range_samplers, 1, true, kMaxSamplers),
  };
  RenderPushConstantRange push(0, 0, 0, kPushBytes,
                               RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::GEOMETRY |
                                   RenderShaderStageFlag::PIXEL);
  layout_ = device_->createPipelineLayout(RenderPipelineLayoutDesc(&push, 1, sets, 4, false, true));
  set_2d_ = device_->createDescriptorSet(sets[0]);
  set_3d_ = device_->createDescriptorSet(sets[1]);
  set_cube_ = device_->createDescriptorSet(sets[2]);
  set_samplers_ = device_->createDescriptorSet(sets[3]);
  if (!layout_ || !set_2d_ || !set_3d_ || !set_cube_ || !set_samplers_) {
    REXLOG_ERROR("Renut: Plume pipeline layout unavailable");
    return false;
  }

  // Descriptor 0 is the fallback the rest of the heap falls back to; the
  // samplers a draw actually asks for are built on demand from its fetch
  // constants.
  samplers_[kSamplerDefault] = device_->createSampler(RenderSamplerDesc());
  if (!samplers_[kSamplerDefault]) {
    REXLOG_ERROR("Renut: Plume default sampler unavailable");
    return false;
  }
  set_samplers_->setSampler(kSamplerDefault, samplers_[kSamplerDefault].get());
  next_sampler_ = kSamplerDefault + 1;

  {
    const uint64_t total_ram = renutPhysicalMemoryBytes();
    const int32_t mode = REXCVAR_GET(renut_plume_low_memory);
    low_memory_ = mode < 0 ? total_ram < (6ull << 30) : mode > 0;
    if (low_memory_) {
      // Showdown Town has peaked at 126 MB of ring a frame on a big device; a
      // slice this size overflows there and costs a drain of the GPU each
      // time, which is the price of not holding 256 MB of RAM for it.
      upload_slice_size_ = 64ull << 20;
      kept_budget_ = 96ull << 20;
      max_texture_dim_ = 512;
      texture_budget_ = 256ull << 20;
    }
    if (const int32_t dim = REXCVAR_GET(renut_plume_texture_max_dim); dim > 0) max_texture_dim_ = uint32_t(dim);
    REXLOG_INFO("Renut: Plume memory profile {}: {} MB of RAM, ring {} MB a slice, kept streams {} MB, textures "
                "{} MB over {} px",
                low_memory_ ? "low" : "full", total_ram >> 20, upload_slice_size_ >> 20, kept_budget_ >> 20,
                texture_budget_ >> 20, max_texture_dim_);
  }
  {
    // Adreno has BC1-BC5 sampling, Mali does not: asked of the driver, since
    // Plume does not report it.
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(static_cast<VulkanDevice*>(device_)->physicalDevice, &features);
    bc_supported_ = features.textureCompressionBC == VK_TRUE;
    // ARM's vendor id.
    trail_device_default_ = static_cast<VulkanDevice*>(device_)->physicalDeviceProperties.vendorID == 0x13B5;
    trail_ = trail_device_default_;
  }
  bc_mode_ = uint32_t(std::clamp(REXCVAR_GET(renut_plume_bc_textures), 0, 1));
  REXLOG_INFO("Renut: Plume BC textures: device {}, mode {}", bc_supported_ ? "samples BC1-BC5" : "has no BC",
              bc_mode_ ? "on" : "off");
  texture_quality_ = uint32_t(std::clamp(REXCVAR_GET(renut_plume_texture_quality), 0, 2));
  shader_cache_ = REXCVAR_GET(renut_plume_shader_cache);
  key_reuse_ = REXCVAR_GET(renut_plume_key_reuse);
  REXLOG_INFO("Renut: Plume texture quality: {}",
              texture_quality_ == 0 ? "full" : texture_quality_ == 1 ? "half" : "quarter");
  shadow_scale_ = 4 - uint32_t(std::clamp(REXCVAR_GET(renut_plume_shadow_quality), 0, 2));
  REXLOG_INFO("Renut: Plume shadow map resolution: {}/4 of the guest's", shadow_scale_);
  internal_scale_ = 4 - uint32_t(std::clamp(REXCVAR_GET(renut_plume_internal_resolution), 0, 2));
  REXLOG_INFO("Renut: Plume internal resolution: {}/4 of the guest's", internal_scale_);
  // One slice per frame in flight, after the permanent allocations.
  const uint64_t upload_bytes = upload_slice_size_ * kPlumeFramesInFlight + 4096;
  upload_ = device_->createBuffer(RenderBufferDesc::UploadBuffer(
      upload_bytes, RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX | RenderBufferFlag::CONSTANT |
                        RenderBufferFlag::DEVICE_ADDRESSABLE | RenderBufferFlag::STORAGE));
  if (!upload_) {
    REXLOG_ERROR("Renut: Plume upload buffer unavailable");
    return false;
  }
  upload_data_ = static_cast<uint8_t*>(upload_->map());
  upload_address_ = upload_->getDeviceAddress();
  if (!CreateConstantBuffers()) {
    REXLOG_ERROR("Renut: Plume constant buffers unavailable");
    return false;
  }
  watch_handle_ = memory_->RegisterPhysicalMemoryInvalidationCallback(WatchHitThunk, this);

  {
    auto* vulkan_device = static_cast<VulkanDevice*>(device_);
    timestamp_ns_ = vulkan_device->physicalDeviceProperties.limits.timestampPeriod;
    VkQueryPoolCreateInfo info = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = kMaxTimestamps * kPlumeFramesInFlight;
    if (vkCreateQueryPool(vulkan_device->vk, &info, nullptr, &timestamp_pool_) != VK_SUCCESS) {
      timestamp_pool_ = VK_NULL_HANDLE;
      REXLOG_WARN("Renut: Plume timestamp queries unavailable; no per-pass GPU time");
    }
    info.queryCount = kPairQueries * kPlumeFramesInFlight;
    if (timestamp_pool_ == VK_NULL_HANDLE ||
        vkCreateQueryPool(vulkan_device->vk, &info, nullptr, &pair_pool_) != VK_SUCCESS) {
      pair_pool_ = VK_NULL_HANDLE;
    }
  }
  for (auto& list : lists_) list = queue_->createCommandList();
  list_ = lists_[0].get();
  defer_mode_ = uint32_t(std::clamp(REXCVAR_GET(renut_plume_defer_draws), 0, 2));
  hiz_cond_ = PlumeConditionalRenderingSupported();
  hiz_force_ = getenv("RENUT_HIZ_FORCE") != nullptr;
  hiz_verify_ = getenv("RENUT_HIZ_VERIFY") != nullptr;
  hiz_debug_ = getenv("RENUT_HIZ_DEBUG") != nullptr;
  if (hiz_verify_) {
    VkQueryPoolCreateInfo query_info = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    query_info.queryType = VK_QUERY_TYPE_OCCLUSION;
    query_info.queryCount = kHizMaxQueries * kPlumeFramesInFlight;
    if (vkCreateQueryPool(static_cast<VulkanDevice*>(device_)->vk, &query_info, nullptr, &hiz_query_pool_) !=
        VK_SUCCESS) {
      hiz_query_pool_ = VK_NULL_HANDLE;
    }
  }
  hiz_mode_ = uint32_t(std::clamp(REXCVAR_GET(renut_plume_hiz), 0, 4));
  fetch_spec_mode_ = uint32_t(std::clamp(REXCVAR_GET(renut_plume_fetch_spec), 0, 1));
  depth_strip_mode_ = uint32_t(std::clamp(REXCVAR_GET(renut_plume_depth_strip), 0, 1));
  rel_constants_mode_ = uint32_t(std::clamp(REXCVAR_GET(renut_plume_rel_constants), 0, 2));
  no_shadows_ = REXCVAR_GET(renut_plume_no_shadows);
  no_bloom_ = REXCVAR_GET(renut_plume_no_bloom);
  adaptive_mode_ = uint32_t(std::clamp(REXCVAR_GET(renut_plume_adaptive_patches), 0, 1));
  if (no_shadows_) shadow_min_indices_ = UINT32_MAX;
  fence_ = device_->createCommandFence();

  // Fallback textures: opaque white, for every slot without a texture yet.
  white_2d_ = device_->createTexture(RenderTextureDesc::Texture2D(1, 1, 1, RenderFormat::R8G8B8A8_UNORM));
  white_3d_ = device_->createTexture(RenderTextureDesc::Texture3D(1, 1, 1, 1, RenderFormat::R8G8B8A8_UNORM));
  RenderTextureDesc cube = RenderTextureDesc::Texture(RenderTextureDimension::TEXTURE_2D, 1, 1, 1, 1, 6,
                                                      RenderFormat::R8G8B8A8_UNORM, RenderTextureFlag::CUBE);
  white_cube_ = device_->createTexture(cube);
  RenderTextureViewDesc cube_view;
  cube_view.format = RenderFormat::R8G8B8A8_UNORM;
  cube_view.dimension = RenderTextureViewDimension::TEXTURE_CUBE;
  white_cube_view_ = white_cube_->createTextureView(cube_view);

  rect_list_gs_ = device_->createShader(k_rect_list_geom, sizeof(k_rect_list_geom), "main",
                                        RenderShaderFormat::SPIRV);
  point_list_gs_ = device_->createShader(k_point_list_geom, sizeof(k_point_list_geom), "main",
                                         RenderShaderFormat::SPIRV);

  upload_used_ = 0;
  uint64_t white = Allocate(4 * 6, 256);
  std::memset(Pointer(white), 0xFF, 4 * 6);
  zero_offset_ = Allocate(64, 16);
  std::memset(Pointer(zero_offset_), 0, 64);
  // Everything after these belongs to a frame's slice.
  slice_origin_ = (upload_used_ + 255) & ~255ull;
  L()->begin();
  L()->barriers(RenderBarrierStage::COPY,
                  std::array{RenderTextureBarrier(white_2d_.get(), RenderTextureLayout::COPY_DEST),
                             RenderTextureBarrier(white_3d_.get(), RenderTextureLayout::COPY_DEST),
                             RenderTextureBarrier(white_cube_.get(), RenderTextureLayout::COPY_DEST)}
                      .data(),
                  3);
  auto footprint = RenderTextureCopyLocation::PlacedFootprint(upload_.get(), RenderFormat::R8G8B8A8_UNORM,
                                                              1, 1, 1, 64, white);
  L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(white_2d_.get()), footprint);
  L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(white_3d_.get()), footprint);
  for (uint32_t face = 0; face < 6; ++face) {
    L()->copyTextureRegion(RenderTextureCopyLocation::Subresource(white_cube_.get(), 0, face), footprint);
  }
  L()->barriers(RenderBarrierStage::GRAPHICS,
                  std::array{RenderTextureBarrier(white_2d_.get(), RenderTextureLayout::SHADER_READ),
                             RenderTextureBarrier(white_3d_.get(), RenderTextureLayout::SHADER_READ),
                             RenderTextureBarrier(white_cube_.get(), RenderTextureLayout::SHADER_READ)}
                      .data(),
                  3);
  L()->end();
  const RenderCommandList* lists = list_;
  queue_->executeCommandLists(&lists, 1, nullptr, 0, nullptr, 0, fence_.get());
  queue_->waitForCommandFence(fence_.get());
  for (uint32_t i = 0; i < kMaxTextures2D; ++i) {
    set_2d_->setTexture(i, white_2d_.get(), RenderTextureLayout::SHADER_READ);
  }
  for (uint32_t i = 0; i < kMaxTextures3D; ++i) {
    set_3d_->setTexture(i, white_3d_.get(), RenderTextureLayout::SHADER_READ);
  }
  for (uint32_t i = 0; i < kMaxTexturesCube; ++i) {
    set_cube_->setTexture(i, white_cube_.get(), RenderTextureLayout::SHADER_READ,
                          white_cube_view_.get());
  }
  lookup_budget_us_ = uint64_t(REXCVAR_GET(renut_plume_pipeline_lookup_ms)) * 1000;
  lookup_here_ = lookup_budget_us_ && PlumePipelineLookupSupported();
  REXLOG_INFO("Renut: Plume pipelines: {} compile, {}, boolean registers {}",
              REXCVAR_GET(renut_plume_async_pipelines) ? "async" : "sync",
              lookup_here_ ? fmt::format("cache lookups at the draw up to {} ms a frame", lookup_budget_us_ / 1000)
                           : std::string("no cache lookups at the draw"),
              REXCVAR_GET(renut_plume_bool_spec) ? "specialized" : "read from the shared block");
  validate_windows_ = g_validate_windows;
  dedupe_descriptors_ = g_dedupe_descriptors;
  if (dedupe_descriptors_) REXLOG_INFO("Renut: Plume 2D descriptors are written once per texture (plume-dedupe-desc)");
  writeback_ = REXCVAR_GET(renut_plume_memexport_readback);
  writeback_fast_ = REXCVAR_GET(renut_plume_memexport_fast);
  BeginFrame();
  REXLOG_INFO("Renut: Plume command processor ready ({} frames in flight, upload {} MB at {:X})",
              kPlumeFramesInFlight, upload_bytes >> 20, upload_address_);
  StartRecipes();
  device_watch_stop_ = false;
  device_watch_.thread = std::thread([this] {
    while (!device_watch_stop_.load()) {
      if (PlumeDeviceLost()) {
        ReportDeviceLost();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  });
  return true;
}

// The worker's loop. The lists the game's Direct3D published are run as they
// come (the doorbell wakes it); the primary ring is not read, only kept moving:
// a write pointer that advanced is taken as consumed and the read pointer is
// written back, which is all Direct3D looks at.
void PlumeCommandProcessor::RunWorkerLoop() {
  uint32_t idle = 0;
  while (worker_running_) {
    while (!pending_fns_.empty()) {
      auto fn = std::move(pending_fns_.front());
      pending_fns_.pop();
      fn();
    }
    bool worked = false;
    if (NativeWorkPending()) {
      RunNativeWork();
      worked = true;
    }
    const uint32_t write_ptr_index = write_ptr_index_.load();
    if (write_ptr_index != 0xBAADF00D && read_ptr_index_ != write_ptr_index) {
      read_ptr_index_ = write_ptr_index;
      if (read_ptr_writeback_ptr_) StoreBig(read_ptr_writeback_ptr_, read_ptr_index_);
      worked = true;
    }
    if (worked) {
      idle = 0;
      continue;
    }
    // Out of commands: spin for new ones, since waiting on the event costs more
    // than the wait is worth; it is waited on after a while to let the core rest.
    if (++idle > 500) {
      rex::thread::Wait(write_ptr_index_event_.get(), true, std::chrono::milliseconds(5));
    }
    rex::thread::MaybeYield();
  }
}

void PlumeCommandProcessor::ShutdownContext() {
  SyncRecorder();
  native_d3d::SetWake({});
  device_watch_stop_ = true;
  if (device_watch_.thread.joinable()) device_watch_.thread.join();
  StopPipelineWorker();
  StopRecipes();
  if (list_open_) {
    L()->end();
    list_open_ = false;
  }
  // Frames may still be running; nothing below may be freed under them.
  if (auto* renderer = PlumeRenderer::Get()) renderer->WaitForAllSlots();
  pipelines_.clear();
  standins_.clear();
  pipeline_memo_.Clear();
  framebuffers_.clear();
  ++object_epoch_;
  targets_.clear();
  resolved_.clear();
  InvalidateResolvedLookups();
  ++export_epoch_;
  exports_.clear();
  patch_grids_.clear();
  if (watch_handle_) {
    memory_->UnregisterPhysicalMemoryInvalidationCallback(watch_handle_);
    watch_handle_ = nullptr;
  }
  ++stream_epoch_;
    kept_streams_.clear();
  kept_memo_.Clear();
  kept_bytes_ = 0;
  for (auto& retired : retired_buffers_) retired.clear();
  for (auto& buffer : writeback_buffers_) buffer.reset();
  for (auto& list : writebacks_) list.clear();
  shader_lookup_.clear();
  shader_memo_.Clear();
  shader_watch_memo_.Clear();
  shader_watch_.clear();
  shader_block_index_.clear();
  shaders_.clear();
  if (timestamp_pool_ != VK_NULL_HANDLE) {
    vkDestroyQueryPool(static_cast<VulkanDevice*>(device_)->vk, timestamp_pool_, nullptr);
    timestamp_pool_ = VK_NULL_HANDLE;
  }
  if (pair_pool_ != VK_NULL_HANDLE) {
    vkDestroyQueryPool(static_cast<VulkanDevice*>(device_)->vk, pair_pool_, nullptr);
    pair_pool_ = VK_NULL_HANDLE;
  }
  if (upload_) upload_->unmap();
  upload_.reset();
  {
    VkDevice vk_device = static_cast<VulkanDevice*>(device_)->vk;
    if (pack_pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(vk_device, pack_pipeline_, nullptr);
    if (pack_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(vk_device, pack_layout_, nullptr);
    if (pack_module_ != VK_NULL_HANDLE) vkDestroyShaderModule(vk_device, pack_module_, nullptr);
    pack_pipeline_ = VK_NULL_HANDLE;
    pack_layout_ = VK_NULL_HANDLE;
    pack_module_ = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < 2; ++i) {
      if (patch_pipelines_[i] != VK_NULL_HANDLE) vkDestroyPipeline(vk_device, patch_pipelines_[i], nullptr);
      if (patch_modules_[i] != VK_NULL_HANDLE) vkDestroyShaderModule(vk_device, patch_modules_[i], nullptr);
      patch_pipelines_[i] = VK_NULL_HANDLE;
      patch_modules_[i] = VK_NULL_HANDLE;
    }
    if (patch_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(vk_device, patch_layout_, nullptr);
    HizDestroy();
    patch_layout_ = VK_NULL_HANDLE;
    for (auto& buffer : patch_out_) buffer.reset();
    stencil_staging_.reset();
    packed_staging_.reset();
    if (constants_pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(vk_device, constants_pool_, nullptr);
    if (constants_set_layout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(vk_device, constants_set_layout_, nullptr);
    }
    constants_pool_ = VK_NULL_HANDLE;
    constants_set_layout_ = VK_NULL_HANDLE;
    constants_set_ = VK_NULL_HANDLE;
  }
}

// Set 4 of the pipeline layout: the stages' constant files and the shared
// block as dynamic uniform buffers over the upload ring. Plume has no dynamic
// descriptors, so the set is built in Vulkan and the layout Plume made is
// rebuilt with it appended - sets 0 to 3 and the push constants unchanged.
bool PlumeCommandProcessor::CreateConstantBuffers() {
  VkDevice vk_device = static_cast<VulkanDevice*>(device_)->vk;
  auto* layout = static_cast<VulkanPipelineLayout*>(layout_.get());
  VkDescriptorSetLayoutBinding bindings[kConstantBindings] = {};
  for (uint32_t i = 0; i < kConstantBindings; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo set_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = kConstantBindings;
  set_info.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(vk_device, &set_info, nullptr, &constants_set_layout_) != VK_SUCCESS) {
    return false;
  }
  std::vector<VkDescriptorSetLayout> set_layouts;
  for (VulkanDescriptorSetLayout* set_layout : layout->descriptorSetLayouts) set_layouts.push_back(set_layout->vk);
  if (set_layouts.size() != 4) return false;
  set_layouts.push_back(constants_set_layout_);
  VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = uint32_t(set_layouts.size());
  layout_info.pSetLayouts = set_layouts.data();
  layout_info.pushConstantRangeCount = uint32_t(layout->pushConstantRanges.size());
  layout_info.pPushConstantRanges = layout->pushConstantRanges.data();
  VkPipelineLayout extended = VK_NULL_HANDLE;
  if (vkCreatePipelineLayout(vk_device, &layout_info, nullptr, &extended) != VK_SUCCESS) return false;
  vkDestroyPipelineLayout(vk_device, layout->vk, nullptr);
  layout->vk = extended;

  VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kConstantBindings};
  VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (vkCreateDescriptorPool(vk_device, &pool_info, nullptr, &constants_pool_) != VK_SUCCESS) return false;
  VkDescriptorSetAllocateInfo allocate = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool = constants_pool_;
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &constants_set_layout_;
  if (vkAllocateDescriptorSets(vk_device, &allocate, &constants_set_) != VK_SUCCESS) return false;
  const VkBuffer ring = static_cast<VulkanBuffer*>(upload_.get())->vk;
  VkDescriptorBufferInfo infos[kConstantBindings];
  static_assert(RENUT_SHARED_HOT_SIZE % 16 == 0 && RENUT_SHARED_SIZE % 16 == 0);
  for (uint32_t i = 0; i < kSharedHotBinding; ++i) infos[i] = {ring, 0, kConstantChunkSize};
  infos[kSharedHotBinding] = {ring, 0, RENUT_SHARED_HOT_SIZE};
  infos[kSharedColdBinding] = {ring, 0, RENUT_SHARED_SIZE - RENUT_SHARED_HOT_SIZE};
  // The whole vertex file, through both of its declarations.
  infos[kRelBinding] = {ring, 0, kStageConstantSize};
  infos[kRelBinding + 1] = {ring, 0, kStageConstantSize};
  VkWriteDescriptorSet writes[kConstantBindings] = {};
  for (uint32_t i = 0; i < kConstantBindings; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = constants_set_;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    writes[i].pBufferInfo = &infos[i];
  }
  vkUpdateDescriptorSets(vk_device, kConstantBindings, writes, 0, nullptr);
  return true;
}

}  // namespace renut::gpu
