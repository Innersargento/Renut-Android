#pragma once
// Types and constants of the GPU backend, hoisted out of its class so that every subsystem
// (and every file) names them the same way.
#include "resources.h"

namespace renut::gpu {

  struct IndexBufferInfo {
    xenos::IndexFormat format = xenos::IndexFormat::kInt16;
    xenos::Endian endianness = xenos::Endian::kNone;
    uint32_t count = 0;
    uint32_t guest_base = 0;
    size_t length = 0;
  };

  inline constexpr uint32_t kConstantChunks = 4;

  inline constexpr uint32_t kConstantChunkSize = 1024;

  // ---- Native Direct3D lists ----
  // All drawing comes from the front end's lists (d3d/native/), which run
  // with a register file of their own as the GPU backend reaches the
  // ring segments they go with. The ring's own register file is the GPU's as
  // the CPU sees it (scratch registers, the writeback the callbacks read).
  struct ExecutorBusy {
    ExecutorBusy() { native_d3d::SetExecutorBusy(true); }
    ~ExecutorBusy() { native_d3d::SetExecutorBusy(false); }
  };

  // The vertex buffer behind each stream and the index buffer (kOpStreams,
  // kOpIndices): a stream inside one is kept as that whole buffer.
  struct BufferRange {
    uint32_t base = 0, size = 0;
  };

  struct NativeFrame {
    std::shared_ptr<const native_d3d::List> list;
    size_t pos = 0;
  };

  // The surfaces bound when the lists last sent the target registers
  // (kOpSurfaces): render targets 0-3 and depth.
  struct ListSurface {
    uint32_t object = 0, surface_info = 0, info = 0, dims = 0;
  };

  // Adaptive triangle patches (ExpandAdaptivePatches): patch_levels.comp and
  // patch_expand.comp write each draw's points, indices and indirect
  // arguments into the slot's buffer, filled from its start every frame.
  struct AdaptiveDraw {
    RenderBuffer* buffer = nullptr;
    uint64_t points_offset = 0, points_bytes = 0, args_offset = 0;
  };

  // The last expansion, which the same draw repeated for the next band of a
  // predicated frame takes as it is while no export has happened in between.
  struct PatchReuse {
    uint64_t frame = UINT64_MAX, factors = 0, exports = 0;
    uint32_t patches = 0, levels = 0;
    AdaptiveDraw draw;
  };

  struct KeptStream {
    std::unique_ptr<RenderBuffer> buffer;
    uint32_t base = 0, size = 0;
    uint64_t last_used = 0, dropped_frame = 0, made_frame = 0;
    uint32_t drops = 0, endian = 0;
    bool valid = false;
  };

  struct StreamMemo {
    uint32_t words[2] = {};
    uint64_t native_base = 0, native_size = 0;
    uint64_t repack_signature = 0;
    uint64_t epoch = 0, generation = 0, export_epoch = 0;
    RenderBuffer* buffer = nullptr;
    uint64_t offset = 0;
    bool bound = false;
  };

  // Memory export (renutMemExport in shader_common.h). A range of guest memory
  // a vertex shader has written lives in a buffer of its own from then on, and
  // later draws that fetch from it read that buffer: guest memory never sees
  // what the GPU wrote. The CPU's own writes there still win - each frame the
  // range is compared with the guest bytes it was last in step with, and what
  // changed is copied in.
  struct ExportRange {
    uint32_t base = 0, size = 0;
    xenos::Endian endian = xenos::Endian::k8in32;
    std::unique_ptr<RenderBuffer> buffer;
    uint64_t address = 0;
    std::vector<uint8_t> snapshot;
    uint64_t checked_frame = UINT64_MAX, exported_frame = 0;
    // Since the last ExportBarrier: written by a shader or a copy, read by a draw.
    bool written = false, read = false;
  };

  struct Writeback {
    uint32_t base = 0, size = 0;
    uint64_t offset = 0;
    const RenderBuffer* buffer = nullptr;
  };

  // The expansion of a patch draw, per shape and level: every point of a
  // patch's grid once, and the triangles over them as indices. It depends on
  // nothing but the shape, the level and how many patches there are, so it is
  // built once and kept, for as many patches as the largest draw has asked.
  // Built afresh every draw it was the command list thread's largest write
  // (tens of MB a frame in the water) and, unindexed, ran the guest's domain
  // shader six times for every point two triangles share.
  struct PatchGrid {
    std::unique_ptr<RenderBuffer> points, indices;
    uint32_t patches = 0;
    uint32_t points_per_patch = 0, indices_per_patch = 0;
  };

  // Everything LayoutOf works out from a fetch constant, kept per constant.
  struct FetchLayout {
    uint64_t key = 0;
    uint32_t levels = 0, top_level = 0;
    std::array<TextureLevel, 14> levels_layout{};
    std::array<uint32_t, 14> starts{};
    bool usable = false;
    // The guest reads the top level alone (interface, fonts, post): never reduced.
    bool base_map = false;
  };

  // LoadShader's answers this frame, by microcode address: {dwords, shader}.
  // Across frames (renut_plume_shader_cache): the shader found at a guest
  // address and length stays that shader until the guest writes under it. The
  // pages are write-watched like a kept stream's, and an entry written over
  // three times is treated as dynamic (the ring a patched shader is copied
  // into) and goes back to hashing, unwatched.
  struct ShaderWatch {
    HostShader* shader = nullptr;
    uint32_t base = 0, bytes = 0;
    bool valid = false;
    uint32_t drops = 0;
    uint64_t dropped_frame = 0;
  };

  // The guest bytes each traced draw's streams held when it was drawn, checked
  // again at the swap: a stream the CPU rewrote after the draw was read early.
  struct TracedStream {
    uint32_t draw, stream, address;
    std::vector<uint8_t> bytes;
  };

  // Debug: <files>/plume-const holds lines "ps|vs <register> x y z w", float
  // constants written over the guest's before every draw - to try another
  // value (the water picks its tile layout from ps c222) without a rebuild.
  // A line "bool <word> <set hex> <clear hex>" ors and clears bits of that
  // boolean-register word instead.
  struct ConstOverride {
    bool pixel;
    uint32_t reg;
    float value[4];
    bool bool_word = false;
    uint32_t set = 0, clear = 0;
  };

  // MSAA off: the game still records some draws once per band mask - the
  // screen-space shadow multiply as masks ...03 then ...0C, the water's patches
  // as 3 then C - and with a single tile every predicated packet runs
  // (SetSingleBinTiling), so the second copy drew the same thing over the first:
  // shadows applied twice. A predicated draw whose whole state matches an
  // earlier one of the frame under a mask that shares no tile with its own is
  // that copy, and is dropped. <files>/plume-dedupe "0" turns it off.
  struct TwinEntry {
    uint64_t sig = 0, mask = 0, frame = UINT64_MAX;
  };

  // The three constant blocks a draw hands the shaders, kept between draws.
  //
  // The guest rewrites a few registers between draws and leaves the rest
  // alone, so nearly every draw was copying the same 10 KB into a fresh
  // stretch of the ring. Pointing it at the block the last draw left there
  // saves the copy, and - what this is really for - lets the GPU read lines it
  // already has: every constant a recompiled shader reads is a load from this
  // buffer (cReg in shader_common.h), so a block per draw turns every one of
  // those loads into a miss.
  //
  // The shadow copies are what the comparison reads. The ring itself is
  // write-combined memory, which is slow to read back.
  inline constexpr size_t kStageConstantSize = 4096;

  // Set 4: the vertex blocks, the pixel blocks, then the shared block's two
  // halves, then twice the vertex file as one block (g_VertexShaderConstantRel:
  // bindings 10 and 11 read the same bytes, 11 declared far larger).
  inline constexpr uint32_t kSharedHotBinding = 2 * kConstantChunks;

  inline constexpr uint32_t kSharedColdBinding = kSharedHotBinding + 1;

  inline constexpr uint32_t kRelBinding = kSharedColdBinding + 1;

  inline constexpr uint32_t kConstantBindings = kRelBinding + 2;

  struct VertexBinding {
    const RenderBuffer* buffer = nullptr;
    uint64_t offset = UINT64_MAX;
  };

  // One command list per frame in flight, and `list_` is the one being
  // recorded. The slot also owns the slice of the upload ring that list reads
  // from and the host resources it retired, all released together once the
  // renderer's fence for that slot has been waited on.
  // GPU time per render pass, from Vulkan timestamps written at every pass
  // boundary. setFramebuffer ends the active pass before it binds the new one
  // and the pass only reopens at the next draw, so those points are outside a
  // render pass - which is where a timestamp on a tiler means anything. Read
  // back once the slot's fence has been waited on, so nothing stalls for it.
  inline constexpr uint32_t kMaxTimestamps = 512;

  struct PassMark {
    uint32_t width = 0, height = 0, colors = 0, draws = 0;
    bool depth = false;
    // A resolve's copies run between two framebuffer marks, so without a mark
    // of their own their cost is charged to whatever pass preceded them.
    // 1 = the barrier that ends the open pass, 2 = the copies themselves.
    uint8_t resolve = 0;
  };

  struct PassTotal {
    uint64_t ns = 0, passes = 0, draws = 0;
  };

  // Where the GPU's time goes, per (vertex, pixel) shader pair. Debug only:
  // <files>/plume-pairprof holds "<mode> <frames>" and is consumed when read.
  // The next `frames` frames get two timestamps around every draw - from before
  // its state changes to after the draw call - and the sums per pair are
  // written to <files>/plume-pairprof.txt once the last of them has been read
  // back. Mode 1 stamps the top of the pipe, which reads the command
  // processor's clock without waiting for the GPU: when the GPU is the
  // bottleneck the processor stalls on whichever draw cannot be taken in, so
  // the spans add up to throughput cost with the draws still overlapping. Mode
  // 2 stamps the bottom, which drains the pipe at both ends and so measures each
  // draw alone, latency included. The per-pass timestamps above are separate.
  inline constexpr uint32_t kPairQueries = 32768;

  struct PairMark {
    uint64_t vs = 0, ps = 0;
    uint32_t count = 0;
    uint8_t colors = 0, depth = 0, kind = 0, switched = 0;
  };

  struct PairTotal {
    uint64_t ns = 0, draws = 0, vertices = 0, switches = 0;
    uint8_t colors = 0, depth = 0;
  };

  // Deferred draws. IssueDraw gathers what the Vulkan calls of a draw need into
  // a packet. A draw that cannot change the picture when it is reordered
  // against its neighbours waits in a window, and the window is recorded
  // grouped by pipeline when it closes, so that the GPU swaps programs once per
  // group instead of once per draw. Everything else that records into the list
  // goes through L(), which closes the window first, so the command stream
  // differs from the immediate one only inside a window and nothing is ever
  // recorded after a barrier, copy or clear it used to precede.
  struct DeferredSlot {
    RenderInputSlot slot;
    RenderVertexBufferView view;
  };

  struct DrawPacket {
    // kPatchIndirect: an adaptive patch draw, whose index count the GPU wrote
    // at indirect_offset of index_buffer (ExpandAdaptivePatches).
    enum class Kind : uint8_t { kNone, kIndexed, kPatch, kPlain, kPatchIndirect };
    RenderFramebuffer* framebuffer = nullptr;
    uint32_t pass_colors = 0;
    bool pass_depth = false;
    RenderPipeline* pipeline = nullptr;
    uint64_t interface_key = 0;
    bool blend_constants = false;
    bool stencil = false;
    float blend_factor[4] = {};
    uint32_t stencil_ref = 0, stencil_mask = 0, stencil_write_mask = 0;
    uint64_t push[3] = {};
    uint32_t instances = 1;
    uint32_t constant_offsets[kConstantBindings] = {};
    RenderViewport viewport;
    RenderRect scissor;
    uint32_t first_slot = 0, slot_count = 0;
    Kind kind = Kind::kNone;
    RenderBuffer* index_buffer = nullptr;
    RenderFormat index_format = RenderFormat::R16_UINT;
    uint32_t count = 0, first = 0;
    int32_t base_vertex = 0;
    uint64_t indirect_offset = 0;
    // Only for the pair profile, and the trail a lost device prints.
    uint64_t vs_hash = 0, ps_hash = 0;
    uint64_t s0_addr = 0, s10_addr = 0;
    uint32_t s0_size = 0, s10_size = 0;
    // The device address and size of each of the 17 streams the shaders read by address.
    uint64_t stream_addr[17] = {};
    uint32_t stream_size[17] = {};
    // renut_plume_zero_census: the draw's occlusion query.
    uint32_t census_query = UINT32_MAX;
  };

  // The draws recorded last, for the report a lost device prints (see
  // ReportDeviceLost). Written by this thread only; the watcher reads it after
  // the GPU has already faulted, so a torn entry costs a wrong line at worst.
  struct RecentDraw {
    uint64_t frame = 0, vs = 0, ps = 0, pipeline = 0, framebuffer = 0, s0_addr = 0, s10_addr = 0;
    uint64_t pair_first_frame = 0;  // the frame this vertex/pixel pair was first recorded in
    uint32_t kind = 0, count = 0, first = 0, s0_size = 0, s10_size = 0;
    // Everything the GPU reads for this draw by address: the streams, the vertex
    // buffers the input assembler walks and the index range.
    uint64_t stream_addr[17] = {};
    uint32_t stream_size[17] = {};
    uint64_t index_addr = 0;
    uint32_t index_bytes = 0, index_size = 0;
    int32_t base_vertex = 0;
    struct VertexRange {
      uint64_t addr = 0;
      uint32_t size = 0, slot = 0;
    } vertex[6];
    uint32_t vertex_count = 0;
  };

  inline constexpr size_t kRecentDraws = 6144;

  // Joins on destruction, so a processor that never reaches ShutdownContext
  // does not terminate the process with a running thread.
  struct WatchThread {
    std::atomic<bool>& stop;
    std::thread thread;
    explicit WatchThread(std::atomic<bool>& flag) : stop(flag) {}
    ~WatchThread() {
      stop = true;
      if (thread.joinable()) thread.join();
    }
  };

  // Debug (files/plume-skin-skip: hex vertex shader hashes): draws of those skinned shaders are dropped,
  // and every skinned vertex shader seen is counted and listed (LogSkinned) - a bisect of the culprit.
  struct SkinnedSeen {
    uint64_t draws = 0, first_frame = 0, last_frame = 0;
  };

  inline constexpr size_t kMaxDeferred = 384;

  struct HizModelEntry {
    std::shared_ptr<renut::hiz::Model> model;
    bool validated = false;
  };

  // An object-space box of the fetched element over the vertices a draw reads,
  // kept while guest memory under it is not written.
  struct HizBox {
    bool valid = false;
    float lo[4] = {}, hi[4] = {};
    uint32_t vertex_base = 0, vertex_size = 0, index_base = 0, index_size = 0;
    uint32_t first_vertex = 0, last_vertex = 0;
    uint64_t checksum = 0;
    uint64_t checked_frame = 0;
  };

  // How often draws of a mesh (all of its instances together) were found outside
  // the frustum, decayed: a mesh that is almost never culled is only sampled
  // (HizCullDraw), because its tests cost more than they save. Direct-mapped by a
  // key of the model, the fetch range and the index range; a collision restarts it.
  struct HizCold {
    uint64_t key = 0;
    uint32_t seen = 0, culled = 0, phase = 0;
  };

  // A direct-mapped front for hiz_boxes_: the same mesh and index range come
  // back draw after draw and frame after frame. Entries die with a generation
  // bump whenever a box is erased.
  struct HizFront {
    uint64_t key = 0;
    const HizBox* box = nullptr;
    uint32_t generation = 0;
  };

  // ---- Recording thread (files/plume-recthread) ----
  // The draws of a frame are decoded on this thread (IssueDraw makes a packet
  // out of the guest's state) and recorded on another: EmitDraw's Vulkan calls,
  // most of which are the driver copying its state, were about a third of this
  // thread. Packets go to the recorder in batches, in the order they would have
  // been recorded. Everything else that records, or that reads what recording
  // writes (the bound state, the pass marks), goes through L() or SyncRecorder(),
  // which waits for the recorder to be done: from this thread's point of view
  // the list is still recorded in order and by one thread at a time.
  struct RecJob {
    std::vector<DrawPacket> packets;
    std::vector<DeferredSlot> slots;
  };

  inline constexpr size_t kRecBatch = 48;

  // What EmitDraw counts: written by the recorder, folded into stats_ at the
  // report (after a sync).
  struct RecStats {
    uint64_t drawn = 0, pipeline_binds = 0, passes = 0;
  };

  struct RecWorker {
    std::thread thread;
    std::mutex* mutex = nullptr;
    std::condition_variable* cv = nullptr;
    bool* stop = nullptr;
    ~RecWorker() {
      if (!thread.joinable()) return;
      {
        std::lock_guard<std::mutex> lock(*mutex);
        *stop = true;
      }
      cv->notify_all();
      thread.join();
    }
  };

  struct PipelineJob {
    RenderGraphicsPipelineDesc desc;
    std::vector<RenderInputElement> elements;
    std::vector<RenderInputSlot> slots;
    std::vector<RenderSpecConstant> constants;
    std::unique_ptr<RenderPipeline> result;
    std::atomic<bool> done{false};
    uint64_t duration_us = 0;
    // Pipelines that differ only in the boolean registers share one.
    uint64_t family = 0;
    // The shaders and their specialization: what the driver compiles and
    // caches. Pipelines that differ in anything else (blending, depth, targets,
    // vertex layout) share the compiled program (ProgramKey).
    uint64_t program = 0;
    // The driver's cache was already asked for it and did not have it.
    bool looked_up = false;
    // Nothing stands in for its draw, which is missing from the screen until
    // the pipeline is ready.
    bool urgent = false;
    // How it ended: found in the cache rather than compiled; and whether it
    // waited for a sibling's compile of its program first.
    bool found = false, waited = false;
    // How much its draw is missed while it waits: the draws that went without
    // it, weighed by kind and size, and the last frame one did. The urgent queue is served by these rather than in order.
    std::atomic<uint32_t> demand{0};
    std::atomic<uint64_t> wanted_frame{0};
    // For the log when the driver refuses to build it.
    uint64_t vs_hash = 0;
    uint64_t ps_hash = 0;
  };

  // Debug (files/plume-dedupe-desc): a 2D descriptor already holding this texture and view is not
  // written again. The set is bound whole and read by the GPU when the draws run, so a write into
  // it while an earlier frame is still running races with that frame's reads, whatever it writes.
  struct WrittenDescriptor {
    const RenderTexture* texture = nullptr;
    const RenderTextureView* view = nullptr;
  };

  struct ResolvedLookup {
    uint32_t address = 0;
    uint64_t serial = 0;
    Resolved* value = nullptr;
  };

  inline constexpr uint32_t kResolvedLookupSize = 1024;

  // Object identity (files/plume-objkeys): what the six words of a texture
  // fetch constant - the D3D texture's header as the guest bound it - resolved
  // to, kept per fetch slot. The game binds the same few textures to the same
  // slots draw after draw, and hashing the six words and walking two maps to
  // learn that was a visible slice of the command thread. Valid while
  // object_epoch_ is what it was: bumped wherever a layout, a texture or a
  // framebuffer these point at can go away.
  struct SlotObject {
    uint32_t words[6] = {};
    uint64_t epoch = 0;
    const FetchLayout* layout = nullptr;
    GuestTexture* texture = nullptr;
  };

  // The last framebuffer asked for and the images it was made from.
  struct LastFramebuffer {
    const RenderTexture* images[5] = {};
    uint32_t color_count = UINT32_MAX;
    uint64_t epoch = 0;
    RenderFramebuffer* framebuffer = nullptr;
  };

  // Streams by what their fetch constant says, across slots and draws: the
  // per-slot memo only helps a slot whose buffer does not change between
  // draws, and a mesh drawn in the colour pass and in each shadow cascade
  // comes back to different slots' memos. An entry is trusted for the frame it
  // was made in, so the kept stream's age is still refreshed once a frame.
  struct PooledStream {
    StreamMemo memo;
    uint64_t frame = UINT64_MAX;
  };

  inline constexpr uint32_t kStreamPoolSize = 2048;

}  // namespace renut::gpu
