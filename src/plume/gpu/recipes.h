#pragma once
// Pipeline recipes: the file the pipelines are recorded in, precompilation and prediction.
#include "shader_store.h"

namespace renut::gpu {


// Every pipeline a draw asks for is appended to <files>/plume-recipes.bin, its
// shaders named by container hash, and the next start builds them all before a
// scene may load - UnleashedRecomp's method: it holds its loading screen until
// the pipelines it predicted are compiled, so a scene is never seen before they
// exist. N&B's engine is not known well enough to predict them, so what the
// game used before stands in for the prediction. The file is not part of the
// driver's cache, which "Apagar cache de shaders" deletes; this list survives it.

// Bump when what a recipe holds changes: the file is then started again.
constexpr uint32_t kRecipeVersion = 1;
constexpr uint32_t kRecipeMagic = 0x52504E52;  // "RNPR"

struct RecipeHeader {
  uint32_t magic = kRecipeMagic;
  uint32_t version = kRecipeVersion;
};

// Which of a vertex shader's modules (CompiledShader) the pipeline was made with.
enum : uint8_t { kRecipeModule, kRecipeFetchSpec, kRecipeDepth, kRecipeDepthFetchSpec };

struct Recipe {
  uint64_t vs = 0;     // container hash of the vertex shader, or of its domain form
  uint64_t ps = 0;     // 0: no fragment shader
  uint8_t domain = 0;  // vs is a domain form (built without vertex inputs)
  uint8_t form = kRecipeModule;
  uint8_t gs = 0;      // 0 none, 1 rectangle list, 2 point list
  RenderGraphicsPipelineDesc desc;  // shader, layout and array pointers unset
  std::vector<RenderInputElement> elements;
  std::vector<RenderInputSlot> slots;
  std::vector<RenderSpecConstant> constants;
};

struct RecipeWriter {
  static constexpr bool kReading = false;
  std::string& out;
  template <typename T>
  void operator()(T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    out.append(reinterpret_cast<const char*>(&value), sizeof(T));
  }
};

struct RecipeReader {
  static constexpr bool kReading = true;
  const uint8_t* at;
  const uint8_t* end;
  bool ok = true;
  template <typename T>
  void operator()(T& value) {
    if (size_t(end - at) < sizeof(T)) {
      ok = false;
      return;
    }
    std::memcpy(&value, at, sizeof(T));
    at += sizeof(T);
  }
};

// Field by field rather than the desc's bytes: its padding is not zeroed, and
// the bytes written are also the key a draw finds its built pipeline by.
template <typename Io>
bool VisitRecipe(Io& io, Recipe& r) {
  io(r.vs);
  io(r.ps);
  io(r.domain);
  io(r.form);
  io(r.gs);
  RenderGraphicsPipelineDesc& d = r.desc;
  io(d.depthFunction);
  io(d.depthClipEnabled);
  io(d.depthBias);
  io(d.depthBiasClamp);
  io(d.slopeScaledDepthBias);
  io(d.dynamicDepthBiasEnabled);
  io(d.depthEnabled);
  io(d.depthWriteEnabled);
  io(d.stencilEnabled);
  io(d.stencilReadMask);
  io(d.stencilWriteMask);
  io(d.stencilReference);
  for (RenderStencilFaceDesc* face : {&d.stencilFrontFace, &d.stencilBackFace}) {
    io(face->passOp);
    io(face->failOp);
    io(face->depthFailOp);
    io(face->compareFunction);
  }
  io(d.multisampling.sampleCount);
  io(d.multisampling.sampleLocationsEnabled);
  for (RenderMultisamplingLocation& location : d.multisampling.sampleLocations) {
    io(location.x);
    io(location.y);
  }
  io(d.alphaToCoverageEnabled);
  io(d.primitiveTopology);
  io(d.cullMode);
  io(d.frontFace);
  io(d.renderTargetCount);
  if (d.renderTargetCount > RenderGraphicsPipelineDesc::MaxRenderTargets) return false;
  for (uint32_t i = 0; i < d.renderTargetCount; ++i) {
    io(d.renderTargetFormat[i]);
    RenderBlendDesc& b = d.renderTargetBlend[i];
    io(b.blendEnabled);
    io(b.srcBlend);
    io(b.dstBlend);
    io(b.blendOp);
    io(b.srcBlendAlpha);
    io(b.dstBlendAlpha);
    io(b.blendOpAlpha);
    io(b.renderTargetWriteMask);
  }
  io(d.logicOpEnabled);
  io(d.logicOp);
  io(d.depthTargetFormat);
  auto array = [&](auto& items, uint32_t limit, auto&& each) {
    uint32_t count = uint32_t(items.size());
    io(count);
    if constexpr (Io::kReading) {
      if (!io.ok || count > limit) return false;
      items.resize(count);
    }
    for (auto& item : items) each(item);
    return true;
  };
  if (!array(r.elements, 64, [&](RenderInputElement& e) {
        io(e.semanticIndex);
        io(e.location);
        io(e.format);
        io(e.slotIndex);
        io(e.alignedByteOffset);
      })) {
    return false;
  }
  if (!array(r.slots, 64, [&](RenderInputSlot& s) {
        io(s.index);
        io(s.stride);
        io(s.classification);
      })) {
    return false;
  }
  if (!array(r.constants, 1024, [&](RenderSpecConstant& c) {
        io(c.index);
        io(c.value);
      })) {
    return false;
  }
  if constexpr (Io::kReading) return io.ok;
  return true;
}

// What the driver compiles for a recipe: recipes that share it differ only in
// state it links (Turnip's cache is keyed by shaders and specialization), so
// one compile serves them all and the rest are found in the cache.
uint64_t RecipeProgram(const Recipe& r);

// A scene tag in the file: a record whose size has this bit set holds the
// recipe's key (64 bits) and the scene it was drawn in (32) instead of a recipe.
constexpr uint32_t kRecipeTagBit = 0x80000000u;
constexpr uint32_t kRecipeTagSize = 12;

inline uint64_t SceneTagKey(uint64_t recipe, uint32_t scene) {
  uint64_t key = recipe;
  HashCombine(key, scene);
  return key;
}

// The precompilation, shared by its threads and the loading-screen hold
// (PlumeSceneRequested / PlumeSceneReady, driven by game_fixes.cpp). A group is
// the recipes of one program; a scene's groups are those its pipelines are in.
struct PrecompileState {
  enum : uint8_t { kPending, kTaken, kDone };
  std::mutex mutex;
  std::condition_variable changed;
  // Under mutex: each group's state and size in recipes, the groups a scene
  // load asked for first, the next group in file order, and each scene's groups.
  std::vector<uint8_t> state;
  std::vector<uint32_t> group_size;
  std::deque<uint32_t> urgent, background;
  std::unordered_map<uint32_t, std::vector<uint32_t>> scene_groups;
  // Under mutex: the groups of the scene being loaded that are not built yet,
  // how many recipes it has and has left, when it was asked for and when one
  // of its groups was last finished.
  std::vector<uint32_t> scene_missing;
  uint32_t scene_total = 0, scene_left = 0;
  std::chrono::steady_clock::time_point scene_asked, scene_moved;
  std::atomic<uint32_t> done{0}, total{0};
  std::atomic<bool> running{false};
  // From a scene request until the game's loading screen lets go: the threads
  // build at full speed then, behind the game otherwise (UnleashedRecomp
  // raises its compile threads while loading and lowers them in play).
  std::atomic<bool> loading{false};
  // The threads run and take predicted programs (StartRecipes).
  std::atomic<bool> predicting{false};
  // Programs a draw made a new pipeline of, and how many of them had been
  // predicted or recorded ahead (TakeRecipe), for the log.
  std::atomic<uint32_t> draw_programs{0}, draw_programs_ahead{0};
  // The scene the game asked for last (0 before the first: the boot); the
  // pipelines drawn are tagged with it.
  std::atomic<uint32_t> scene{0};
  // Under mutex: every recipe to build and the groups of their indices (deques:
  // a group added while the threads run moves nothing), whether a group is
  // built only to fill the driver's cache (a predicted program: no draw asks
  // for the fixed state it was built with, so its pipeline is dropped), the
  // programs already in a group, the threads building one, and the batch the
  // threads are on (when it started, how many failed).
  std::deque<std::pair<uint64_t, Recipe>> recipes;
  std::deque<std::vector<uint32_t>> groups;
  std::vector<uint8_t> warm_only;
  std::unordered_set<uint64_t> programs;
  uint32_t busy = 0;
  std::chrono::steady_clock::time_point batch_start;
  uint32_t batch_failed = 0;

  // Under mutex: a group of one program's recipes. Returns its index.
  uint32_t AddGroup(std::vector<std::pair<uint64_t, Recipe>>&& items, bool warm) {
    const uint32_t g = uint32_t(groups.size());
    groups.emplace_back();
    for (auto& item : items) {
      groups.back().push_back(uint32_t(recipes.size()));
      recipes.push_back(std::move(item));
    }
    state.push_back(kPending);
    group_size.push_back(uint32_t(groups.back().size()));
    warm_only.push_back(warm);
    total += group_size.back();
    if (!running) {
      running = true;
      batch_start = std::chrono::steady_clock::now();
      batch_failed = 0;
    }
    return g;
  }

  // Under mutex. What a scene being loaded asks for (urgent) - its recorded
  // groups and those predicted from the models it loads - and, for the one
  // thread that works during play (`play`), the programs of models streamed in
  // meanwhile (background). The rest of the recipe list waits for the scene
  // that needs it: built all at start, it took the game's CPU for minutes of
  // the first play. UnleashedRecomp likewise compiles only what the level
  // being loaded needs. UINT32_MAX when there is nothing to take.
  uint32_t TakeGroup(bool play) {
    for (std::deque<uint32_t>* queue : {&urgent, play ? &background : nullptr}) {
      if (!queue) continue;
      while (!queue->empty()) {
        const uint32_t g = queue->front();
        queue->pop_front();
        if (state[g] == kPending) {
          state[g] = kTaken;
          return g;
        }
      }
    }
    return UINT32_MAX;
  }
};
inline PrecompileState g_precompile;

// Shader pairs of loaded models waiting for the command thread
// (PlumePredictPipelines -> PredictPipelines); `queued` counts them until
// they are made into recipes, and a scene does not start while it is not 0.
struct PredictQueue {
  std::mutex mutex;
  std::vector<PlumeShaderPair> pairs;
  std::atomic<uint32_t> queued{0};
};
inline PredictQueue g_predict;

// Decodes the vertex layout a bound vertex shader really fetches. Direct3D
// Which texture fetch constants a shader reads, as a bit per slot. The draw
// path used to ask every one of the 32 slots for its texture, sampler and
// resolved target, on every draw; a shader samples two or three. The constant a
// tfetch names sits in the same bits as a vfetch's, and Direct3D does not
// rewrite it.
uint32_t TextureMask(const gfx::Shader& shader);

// patched the vfetch instructions for the current vertex declaration; the
// recompiler recorded which instruction reads each element (fetchAddress).
//
// Direct3D patches the destination swizzle as well - .yxwz for a 16-bit
// element, which puts back the component order the k8in32 swap reversed,
// .zyxw for a D3DCOLOR, and the constants 0 and 1 for components the vertex
// declaration does not have - and a shader the store found by its loose key
// was recompiled from microcode without that patch (the containers extracted
// from the game's bundles). Such a shader read the halves of every 16-bit pair
// exchanged: the falling leaves' spin axis came out of unit length, so their
// rotation quaternion grew each frame and the leaves with it, into sheets
// across the screen. Colours came out with red and blue exchanged, and alpha
// from memory where the hardware reads 1.
//
// The recompiler records the swizzle it compiled each fetch with (dstSwizzle).
// Every register lane the recompiled code fills from fetched component c the
// live code fills from component l, or with a constant, so component c is
// remapped to l before the recompiled swizzle reads it. Returns the remap as
// the shaders store it (xored with the identity, 0 for none), or ~0u when two
// lanes want one component two ways.
uint32_t FetchRemap(uint32_t compiled, uint32_t live);

void DecodeLiveElements(HostShader& shader, const ShaderCacheEntry* entry,
                        std::vector<LiveElement>& out_elements,
                        std::vector<std::pair<uint32_t, std::array<uint32_t, 4>>>& out_fetches);

void DecodeLiveElements(HostShader& shader);

}  // namespace renut::gpu
