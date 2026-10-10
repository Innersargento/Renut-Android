// Game-specific overrides of recompiled functions. The generated definitions
// are weak symbols, so a strong definition here replaces them at link time.

#include "native_engine.h"
#include "native_probe.h"
#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "plume_gpu.h"

REXCVAR_DEFINE_BOOL(renut_scene_msaa, false, "reNut",
                    "Keep the game's MSAA scene surfaces, which the Xbox 360 renders in two "
                    "or four bands (predicated tiling) with every draw sent once per band. "
                    "Off: one pass, no MSAA - the host never resolved the samples anyway");
REXCVAR_DEFINE_BOOL(renut_shadow_engine_profile, false, "reNut",
                    "Count shadow casters submitted by the game's dynamic and cached shadow passes");
REXCVAR_DEFINE_BOOL(renut_plume_uncap_fps, false, "reNut/Plume",
                    "Use the game's immediate presentation interval instead of its 30 FPS setting");

namespace {

enum class ShadowPass : uint8_t { None, Dynamic, Cached };
thread_local ShadowPass shadow_pass = ShadowPass::None;
std::atomic<uint64_t> shadow_dynamic_passes{0}, shadow_cached_passes{0};
std::atomic<uint64_t> shadow_dynamic_casters{0}, shadow_cached_casters{0};

struct ShadowPassScope {
  explicit ShadowPassScope(ShadowPass next) : previous(shadow_pass) { shadow_pass = next; }
  ~ShadowPassScope() { shadow_pass = previous; }
  ShadowPass previous;
};

// The game's MSAA mode (0 none, 1 2x, 2 4x), read by the scene surface setup
// (sub_823ED2A8) and the tile count (sub_8222A990).
constexpr uint32_t kSceneMsaaMode = 0x82F9EC18;

void ForceSceneSinglePass(uint8_t* base) {
  if (REXCVAR_GET(renut_scene_msaa)) return;
  auto* mode = reinterpret_cast<uint32_t*>(base + kSceneMsaaMode);
  if (*mode != 0) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
      REXLOG_INFO("Renut: scene MSAA mode {} -> none: one pass instead of predicated tiling",
                  __builtin_bswap32(*mode));
    }
    *mode = 0;  // zero reads the same in either byte order
  }
}

}  // namespace

// The game selects a presentation interval through this routine. Index 0 is
// its immediate mode; preserve the original routine's device bookkeeping.
extern "C" void __imp__rex_setParamInterval(PPCContext& __restrict, uint8_t*);
REX_EXTERN(rex_setParamInterval) {
  if (REXCVAR_GET(renut_plume_uncap_fps) && ctx.r3.u32 != 0) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
      REXLOG_INFO("Renut: presentation interval {} -> immediate (FPS unlocked)", ctx.r3.u32);
    ctx.r3.u32 = 0;
  }
  __imp__rex_setParamInterval(ctx, base);
}

// Scene changes, held until the new scene's pipelines exist - as
// UnleashedRecomp holds its loading screen - so it is never drawn without
// them (plume_gpu.cpp: PlumeSceneRequested). The app shows the progress over
// the game meanwhile.
//
// sub_822BC0D8 replaces the running scene: it saves, pops the scene stack and
// pushes (appScenePush) the scene whose asset id r3 points at. Every scene
// change goes through it, supportGameLoadNewScene (LoadShowdownTown too) among
// its callers; that one also passes, in r4, the game ref of what the scene
// loads, whose first word is its asset id, so a world entered through the same
// scene is a scene of its own.
namespace {

thread_local uint32_t pending_scene_ref = 0;

uint32_t GuestWord(uint8_t* base, uint32_t address) {
  if (!address) return 0;
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

}  // namespace

extern "C" void __imp__supportGameLoadNewScene(PPCContext& __restrict, uint8_t*);
REX_EXTERN(supportGameLoadNewScene) {
  pending_scene_ref = GuestWord(base, ctx.r4.u32);
  __imp__supportGameLoadNewScene(ctx, base);
  pending_scene_ref = 0;
}

extern "C" void __imp__sub_822BC0D8(PPCContext& __restrict, uint8_t*);
REX_EXTERN(sub_822BC0D8) {
  const uint32_t asset = GuestWord(base, ctx.r3.u32);
  const uint32_t ref = pending_scene_ref;
  // 0 is the boot's; a scene that hashes to it takes 1.
  uint32_t scene = asset * 0x9E3779B1u ^ ref;
  if (!scene) scene = 1;
  renut::PlumeSceneRequested(scene, asset, ref);
  __imp__sub_822BC0D8(ctx, base);
}

// These are the two shadow-map jobs identified in config/renut_hooks.toml. The
// game still submits every caster normally. This seam lets the native backend
// measure each category before it takes ownership of any draw.
extern "C" void __imp__sub_823B3598(PPCContext& __restrict, uint8_t*);
REX_EXTERN(sub_823B3598) {
  renut::native_probe::EngineEnter(renut::native_probe::kEngine_sub_823B3598, ctx, base);
  if (!REXCVAR_GET(renut_shadow_engine_profile)) {
    __imp__sub_823B3598(ctx, base);
    return;
  }
  ShadowPassScope scope(ShadowPass::Dynamic);
  shadow_dynamic_passes.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_823B3598(ctx, base);
}

extern "C" void __imp__sub_823B3CE0(PPCContext& __restrict, uint8_t*);
REX_EXTERN(sub_823B3CE0) {
  renut::native_probe::EngineEnter(renut::native_probe::kEngine_sub_823B3CE0, ctx, base);
  if (!REXCVAR_GET(renut_shadow_engine_profile)) {
    __imp__sub_823B3CE0(ctx, base);
    return;
  }
  ShadowPassScope scope(ShadowPass::Cached);
  shadow_cached_passes.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_823B3CE0(ctx, base);
}

// sub_823B0698 is the caster draw called by both shadow-map jobs. The tag is
// thread-local because game jobs can run concurrently. No guest state is
// changed, and an unclassified invocation is left alone.
extern "C" void __imp__sub_823B0698(PPCContext& __restrict, uint8_t*);
REX_EXTERN(sub_823B0698) {
  renut::native_probe::EngineEnter(renut::native_probe::kEngine_sub_823B0698, ctx, base);
  if (shadow_pass == ShadowPass::Dynamic) {
    shadow_dynamic_casters.fetch_add(1, std::memory_order_relaxed);
  } else if (shadow_pass == ShadowPass::Cached) {
    shadow_cached_casters.fetch_add(1, std::memory_order_relaxed);
  }
  // Its draws carry the object's bounding sphere to the Plume renderer, which
  // drops shadow casters too small to show in the map (native_engine.h).
  renut::native_engine::SetObject(ctx.r4.u32, base);
  __imp__sub_823B0698(ctx, base);
  renut::native_engine::SetObject(0, base);
}

void renutReportShadowEngineProfile() {
  if (!REXCVAR_GET(renut_shadow_engine_profile)) return;
  const auto dynamic_passes = shadow_dynamic_passes.exchange(0, std::memory_order_relaxed);
  const auto cached_passes = shadow_cached_passes.exchange(0, std::memory_order_relaxed);
  const auto dynamic_casters = shadow_dynamic_casters.exchange(0, std::memory_order_relaxed);
  const auto cached_casters = shadow_cached_casters.exchange(0, std::memory_order_relaxed);
  REXLOG_INFO("Renut: shadow engine over 300 swaps: dynamic {} passes / {} casters, "
              "cached {} passes / {} casters", dynamic_passes, dynamic_casters,
              cached_passes, cached_casters);
}

// Scene render targets @ 0x823ED2A8, rebuilt on every device reset. With MSAA
// the 1280x720 colour and depth surfaces outgrow the 10 MB of EDRAM, so the
// game splits the frame into 2 (2x) or 4 (4x) bands of 384 rows and replays the
// whole scene command buffer per band: in Showdown Town 3,200 of 7,500 draws a
// frame were those replays, and each draw costs ~2.6 us of GPU and ~3.2 us of
// the command thread here. The game has a no-MSAA path of its own - one band,
// single-sampled surfaces at full height - which this selects by clearing the
// mode before the surfaces are sized. Plume never kept the samples: it renders
// one sample per pixel either way.
extern "C" void __imp__sub_823ED2A8(PPCContext& __restrict, uint8_t*);
REX_EXTERN(sub_823ED2A8) {
  ForceSceneSinglePass(base);
  __imp__sub_823ED2A8(ctx, base);
}

// Tile count @ 0x8222A990, asked each frame by the tiling code: keep it on the
// same single band the surfaces were sized for.
extern "C" void __imp__sub_8222A990(PPCContext& __restrict, uint8_t*);
REX_EXTERN(sub_8222A990) {
  ForceSceneSinglePass(base);
  __imp__sub_8222A990(ctx, base);
}

// Scene view job @ 0x823B18C8 (r3 the job; +40 its scene, +80 its band). The
// game renders a view's objects as three jobs: one per MSAA band, each drawing
// the objects that fall inside its band with a projection stretched to that
// band, and one for the whole screen with the objects that span both. The
// band jobs' command buffers are predicated to their bands. With the scene in
// one pass nothing predicates them away, so an object in the lower half is
// drawn over the whole screen with the lower band's projection: vehicles'
// wheels and the crane's claws hang in the air, stretched. The scene's +44
// turns the split on. Clearing it fixed the main view but emptied the water's
// mirrored pass (2 draws instead of ~105: no reflections), so the split stays
// and the band jobs get the view's whole-screen matrix (+128) in place of their
// band's. Keeping the band matrices and giving each band its own viewport
// instead broke the whole picture: not every predicated packet uses a band
// projection.
extern "C" void __imp__sub_823B18C8(PPCContext& __restrict, uint8_t*);
// How the MSAA-off path treats the band split (debug: <files>/plume-keep-split,
// set by Plume): 0 clears the split flag, 1 leaves everything alone, 2 gives the
// band jobs the whole-screen projection, 5 is 2 plus a scissor to each band
// (wrong: with one tile the game predicates almost everything to mask 3).
std::atomic<int> g_renut_scene_split_mode{2};
REX_EXTERN(sub_823B18C8) {
  renut::native_probe::EngineEnter(renut::native_probe::kEngine_sub_823B18C8, ctx, base);
  const int mode = g_renut_scene_split_mode.load(std::memory_order_relaxed);
  if (!REXCVAR_GET(renut_scene_msaa) && (mode == 0 || mode == 2 || mode == 5)) {
    const uint32_t scene = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + ctx.r3.u32 + 40));
    auto* split = reinterpret_cast<uint32_t*>(base + scene + 44);
    if (scene && *split == __builtin_bswap32(1)) {
      if (mode == 0) {
        *split = 0;
      } else {
        // The view's matrices: +128 whole screen, +192 upper band, +256 lower.
        const uint32_t view = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + ctx.r3.u32 + 52));
        uint8_t* block = base + scene + (view << 9);
        std::memcpy(block + 192, block + 128, 64);
        std::memcpy(block + 256, block + 128, 64);
      }
      static std::atomic<bool> logged{false};
      if (!logged.exchange(true)) REXLOG_INFO("Renut: scene {:08X} band split mode {}", scene, mode);
    }
  }
  __imp__sub_823B18C8(ctx, base);
}

// D3D GPU hang handler @ 0x8265AB70, called by the fence watchdog
// (nut_D3D_CBlocker_Check, sub_8264DCD0) after 5000 ticks of KTHREAD
// kernel_time without GPU progress. On the console the GPU never falls that far
// behind, but here the command processor can stall for seconds while shaders
// are translated and the driver compiles pipelines (entering the 3D menu
// creates ~330 at once). The handler then dumps registers, marks the device as
// hung and rewrites the ring read pointer, after which nothing renders. Return
// 0 instead: the watchdog rearms its deadline and keeps waiting for the fence.
REX_EXTERN(sub_8265AB70) {
  static std::atomic<uint32_t> count{0};
  uint32_t n = ++count;
  if (n <= 3 || (n & (n - 1)) == 0) {
    REXLOG_WARN("Renut: GPU fence wait exceeded the D3D hang timeout ({}x); still waiting", n);
  }
  ctx.r3.u64 = 0;
}
