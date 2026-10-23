// Game-specific overrides of recompiled functions. The generated definitions
// are weak symbols, so a strong definition here replaces them at link time.

#include "guest_address.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/hook.h>

#include "d3d/native_d3d.h"
#include "plume/frame_pacing.h"
#include "plume/plume_gpu.h"

REXCVAR_DEFINE_BOOL(renut_scene_msaa, false, "reNut",
                    "Keep the game's MSAA scene surfaces, which the Xbox 360 renders in two "
                    "or four bands (predicated tiling) with every draw sent once per band. "
                    "Off: one pass, no MSAA - the host never resolved the samples anyway");
REXCVAR_DEFINE_INT32(renut_fps_target, 30, "reNut/Plume",
                     "Frame rate the game is held to: 30 is its own (vblank paced), 31-240 presents "
                     "immediately and paces to that rate, 0 is unlimited. Changes live from the sidebar "
                     "(plume/frame_pacing.h)")
    .range(0, 240);
// The old switch, still read so that a renut.toml written before the slider keeps its meaning.
REXCVAR_DEFINE_BOOL(renut_plume_uncap_fps, false, "reNut/Plume",
                    "Deprecated: renut_fps_target = 0 (unlimited) replaces it");

namespace renut::pacing {
void ApplyConfiguredTarget() {
  int fps = REXCVAR_GET(renut_fps_target);
  if (fps == kOriginalFps && REXCVAR_GET(renut_plume_uncap_fps)) fps = kUnlimited;
  SetTarget(fps);
}
}  // namespace renut::pacing

namespace {

// The game's MSAA mode (0 none, 1 2x, 2 4x), read by the scene surface setup
// (sub_823ED2A8) and the tile count (sub_8222A990).
constexpr uint32_t kSceneMsaaMode = 0x82F9EC18;

void ForceSceneSinglePass(uint8_t* base) {
  if (REXCVAR_GET(renut_scene_msaa)) return;
  auto* mode = reinterpret_cast<uint32_t*>(renut::GuestPtr(base, kSceneMsaaMode));
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

// The game selects a presentation interval through this routine. Index 0 is its
// immediate mode; preserve the original routine's device bookkeeping. Anything
// but the game's own 30 FPS asks for the immediate one here, as before the
// frame-rate slider; a target that changes later is also applied per frame at the
// swap's synchronization point (d3d/native/sync.cpp SynchronizeToPresentationInterval).
// What the game ends up with is noted, so that the command thread paces the frames
// itself whenever the vblank does not (frame_pacing.h VblankPaces).
extern "C" void __imp__rex_setParamInterval(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(rex_setParamInterval) {
  if (renut::pacing::ImmediatePresentation() && ctx.r3.u32 != 0) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
      REXLOG_INFO("Renut: presentation interval {} -> immediate (frame-rate target {})", ctx.r3.u32,
                  renut::pacing::Target());
    ctx.r3.u32 = 0;
  }
  renut::pacing::NoteGameInterval(ctx.r3.u32);
  __imp__rex_setParamInterval(ctx, base);
}

// The game's heap free (sub_8223FD48, called from meInternalFree with the heap
// that owns the block) is sometimes handed no heap at all: the lookup of the
// block's owner (sub_82240178) came back empty and the caller went on with a
// null heap, whose lock is then entered at guest address 0x580. On the PC build
// that is a host null dereference about one launch in six, a few seconds in;
// the cause is not known yet. A free that has no heap releases nothing and
// reports success, as a free of NULL does: the block is leaked, not corrupted.
extern "C" void __imp__sub_8223FD48(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_8223FD48) {
  if (ctx.r3.u32 == 0) {
    static std::atomic<uint32_t> reported{0};
    if (reported.fetch_add(1) < 8)
      REXLOG_WARN("Renut: heap free without an owning heap (block {:08X}) skipped", ctx.r5.u32);
    ctx.r3.u32 = 1;
    return;
  }
  __imp__sub_8223FD48(ctx, base);
}

// Photo Mode's capture routine @ 0x82535F20 (r3 the destination texture). It
// resolves the screen into a scratch texture, waits for the copy, and converts
// that texture's memory into the photo's with the CPU (XGCopySurface); the game
// then encodes its JPEG from those texels. Resolves stay on the GPU here, so
// without help the picture is black. While the routine runs, the resolve it
// issues is the one the CPU is about to read: native_d3d marks that record
// (renut_in_photo_capture) and Plume copies that one resolve and no other. A
// routine that is never called, or a resolve outside it, copies nothing - the
// cost while playing is zero.
namespace {
thread_local bool in_photo_capture = false;
}

extern "C" bool renut_in_photo_capture() { return in_photo_capture; }

extern "C" void __imp__sub_82535F20(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_82535F20) {
  in_photo_capture = true;
  __imp__sub_82535F20(ctx, base);
  in_photo_capture = false;
}

// Scene changes: the game's own loading screen stays up until the new scene's
// pipelines exist, as UnleashedRecomp holds its loading screen
// (CGameModeStage::ExitLoading) until its compiles finish, so a scene is never
// drawn without them. The request only moves the scene's pipelines to the front
// of the queue (plume/gpu/host.cpp: PlumeSceneRequested); the hold is further down,
// where the loaded scene would start.
//
// sub_822BC0D8 replaces the running scene: it saves, pops the scene stack and
// pushes (appScenePush) the scene whose asset id r3 points at. Every scene
// change goes through it, supportGameLoadNewScene (LoadShowdownTown too) among
// its callers; that one also passes, in r4, the game ref of what the scene
// loads, whose first word is its asset id, so a world entered through the same
// scene is a scene of its own.
namespace {

thread_local uint32_t pending_scene_ref = 0;
// A scene was asked for since the boot (sub_822BC0D8).
std::atomic<bool> scene_asked_since_boot{false};

uint32_t GuestWord(uint8_t* base, uint32_t address) {
  if (!address) return 0;
  uint32_t value;
  std::memcpy(&value, renut::GuestPtr(base, address), sizeof(value));
  return __builtin_bswap32(value);
}

}  // namespace

extern "C" void __imp__supportGameLoadNewScene(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(supportGameLoadNewScene) {
  pending_scene_ref = GuestWord(base, ctx.r4.u32);
  __imp__supportGameLoadNewScene(ctx, base);
  pending_scene_ref = 0;
}

extern "C" void __imp__sub_822BC0D8(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_822BC0D8) {
  const uint32_t asset = GuestWord(base, ctx.r3.u32);
  const uint32_t ref = pending_scene_ref;
  // 0 is the boot's; a scene that hashes to it takes 1.
  uint32_t scene = asset * 0x9E3779B1u ^ ref;
  if (!scene) scene = 1;
  renut::PlumeSceneRequested(scene, asset, ref);
  scene_asked_since_boot = true;
  __imp__sub_822BC0D8(ctx, base);
}

// The loading screen stays up until the new scene's pipelines are built. A
// scene's per-frame update (sub_8229B330, r3 the scene) starts it when the
// scene is loaded (+16 state 2) and not started yet (+52): it waits there while
// an intro is pending (the byte @ 0x82FAC650 nonzero, the sequence @ 0x82FAC654
// not finished - the Rare logo at boot), then starts the scene, releases the
// loading screen it held (sub_822BBE58) and sets +52. Until the scene's
// pipelines are built, reNut keeps it waiting at that point - the analog of
// UnleashedRecomp's CheckMadeAll hooks - so the game goes on drawing whatever
// it shows while loading (the logo, the closed puzzle between worlds), exactly
// as on a slow disc: with an intro the sequence's state query reports it not
// finished; without one the update sees the scene as started for that call and
// returns before starting it. What the start itself loads is waited for where
// it lets go of the loading screen (sub_822BBE58 below).
namespace {

// The intro pending: a byte (sub_8229B330 tests byte_82FAC650; the bytes after
// it are other flags, so reading it as a word took them for an intro and let
// the scene start under the hold). Its sequence is the word at +4.
constexpr uint32_t kIntro = 0x82FAC650;
constexpr uint32_t kSequenceFinished = 4;

thread_local bool in_scene_start = false;
bool pipeline_hold = false;  // the game's main thread only
// Inside an update that may start the scene (the free path below).
thread_local bool in_scene_update = false;

void StoreGuestWord(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(renut::GuestPtr(base, address), &value, sizeof(value));
}

}  // namespace

extern "C" void __imp__sub_8229B330(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_8229B330) {
  const uint32_t scene = ctx.r3.u32;
  if (GuestWord(base, scene + 52) != 0) {
    __imp__sub_8229B330(ctx, base);
    return;
  }
  // Not started. The update applies the queued state changes first, and a
  // scene object reused for the next scene goes from 0 to loaded (2) and would
  // start within one call: the load itself - its models, whose pipelines are
  // predicted as they load - happens inside it. So the scene may start only in
  // a call it enters loaded, once its pipelines are built.
  if (GuestWord(base, scene + 16) == 2 && renut::PlumeSceneReady()) {
    in_scene_update = true;
    __imp__sub_8229B330(ctx, base);
    in_scene_update = false;
    if (GuestWord(base, scene + 52)) {
      renut::PlumeSceneOpened(pipeline_hold);
      pipeline_hold = false;
    }
    return;
  }
  if (*renut::GuestPtr(base, kIntro)) {
    in_scene_start = true;
    __imp__sub_8229B330(ctx, base);
    in_scene_start = false;
  } else {
    StoreGuestWord(base, scene + 52, 1);
    __imp__sub_8229B330(ctx, base);
    if (GuestWord(base, scene + 52) == 1) StoreGuestWord(base, scene + 52, 0);
  }
  if (GuestWord(base, scene + 52)) {  // started all the same
    renut::PlumeSceneOpened(pipeline_hold);
    pipeline_hold = false;
  } else if (GuestWord(base, scene + 16) == 2 && !pipeline_hold) {
    pipeline_hold = true;
    REXLOG_INFO("Renut: scene {:08X} loaded; its loading screen held for its pipelines", scene);
  }
}

// The start itself builds the world (sub_823C5408): most of a level's models
// load inside it, after the scene was let go above, and their pipelines are
// predicted only then. So the start is held once more where it lets go of the
// loading screen (sub_822BBE58, r4 1: the scene's hold on it), until those are
// built: the game's thread waits here as it did for the load itself. The
// picture stands still meanwhile - the loading screen is drawn by this same
// thread, between frames; leaving the release for a later frame instead made
// the game wait for it and never come back. Not at the boot, whose logo the
// starting scene draws itself. The GPU backend turns the predictions into
// pipelines meanwhile from its own loop, with no frame to wait for.
extern "C" void __imp__sub_822BBE58(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_822BBE58) {
  if (in_scene_update && ctx.r4.u32 != 0 && scene_asked_since_boot.load()) {
    const auto start = std::chrono::steady_clock::now();
    uint32_t polls = 0;
    while (!renut::PlumeSceneReady()) {
      ++polls;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (polls) {
      REXLOG_INFO("Renut: scene start held its loading screen {} ms more for the pipelines of the models it loaded",
                  std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                      .count());
    }
  }
  __imp__sub_822BBE58(ctx, base);
}

// The engine pass a model's records are drawn in. sub_823A9908 (r3 the
// renderer, r4 the model, r5 the pass 0-7) draws the records of one pass of a
// model (sub_823A9A58 -> sub_823A9C38 for each); sub_823A9730 picks the passes
// of a scene phase and sets the render state they draw with around them (pass
// 1 with alpha to coverage, sub_823A7F40), so every record of a pass draws with
// the pass's state. The draws made inside are stamped with it (native_d3d.h,
// EnginePass), and their pipelines teach Plume each pass's states, which the
// prediction below builds the passes' records with.
extern "C" void __imp__sub_823A9908(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_823A9908) {
  uint8_t& pass = renut::native_d3d::EnginePass();
  const uint8_t outer = pass;
  pass = ctx.r5.u32 < 8 ? uint8_t(ctx.r5.u32 + 1) : 0;
  __imp__sub_823A9908(ctx, base);
  pass = outer;
}

// The intro sequence's state, asked by the scene start above.
extern "C" void __imp__sub_8238A418(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_8238A418) {
  __imp__sub_8238A418(ctx, base);
  if (in_scene_start && ctx.r3.u32 == kSequenceFinished) ctx.r3.u64 = kSequenceFinished - 1;
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
REX_HOOK_RAW(sub_823ED2A8) {
  ForceSceneSinglePass(base);
  __imp__sub_823ED2A8(ctx, base);
}

// Tile count @ 0x8222A990, asked each frame by the tiling code: keep it on the
// same single band the surfaces were sized for.
extern "C" void __imp__sub_8222A990(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_8222A990) {
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
REX_HOOK_RAW(sub_823B18C8) {
  const int mode = g_renut_scene_split_mode.load(std::memory_order_relaxed);
  if (!REXCVAR_GET(renut_scene_msaa) && (mode == 0 || mode == 2 || mode == 5)) {
    const uint32_t scene = __builtin_bswap32(*reinterpret_cast<uint32_t*>(renut::GuestPtr(base, ctx.r3.u32 + 40)));
    auto* split = reinterpret_cast<uint32_t*>(renut::GuestPtr(base, scene + 44));
    if (scene && *split == __builtin_bswap32(1)) {
      if (mode == 0) {
        *split = 0;
      } else {
        // The view's matrices: +128 whole screen, +192 upper band, +256 lower.
        const uint32_t view = __builtin_bswap32(*reinterpret_cast<uint32_t*>(renut::GuestPtr(base, ctx.r3.u32 + 52)));
        uint8_t* block = renut::GuestPtr(base, scene + (view << 9));
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
// behind, but here the GPU backend can stall for seconds while shaders
// are translated and the driver compiles pipelines (entering the 3D menu
// creates ~330 at once). The handler then dumps registers, marks the device as
// hung and rewrites the ring read pointer, after which nothing renders. Return
// 0 instead: the watchdog rearms its deadline and keeps waiting for the fence.
REX_HOOK_RAW(sub_8265AB70) {
  static std::atomic<uint32_t> count{0};
  uint32_t n = ++count;
  if (n <= 3 || (n & (n - 1)) == 0) {
    REXLOG_WARN("Renut: GPU fence wait exceeded the D3D hang timeout ({}x); still waiting", n);
  }
  ctx.r3.u64 = 0;
}

// Pipeline prediction, UnleashedRecomp's method: when a model's render data
// loads, every pipeline it will draw with is built before its scene may start.
// A model's render data is a "rendergraph" file (it starts with that name). Its
// draws are 32-byte records (sub_823A9C38: +8 and +12 point at references whose
// first word is the vertex and pixel shader object), reached from the model
// through 8 passes of 16-byte groups; the file registers its vertex shaders in
// the GPU fixup table at +252 (sub_82B67318: +12 the references, +20 how many).
// sub_82B673D0 (r4 the file) runs that fixup once the file is loaded and
// relocated; the records lie between the file and the table, so the scan below
// finds them all (checked on the boot and the title: every record drawn later
// had been found). The engine's draws bind no vertex declaration, so the vertex
// shader runs as its binding 0 holds it - the microcode Plume sees.
namespace {

// Direct3D's view of physical memory (d3d/native/internal.h's Phys).
uint32_t Physical(uint32_t address) { return (address & 0x1FFFFFFF) + (((address >> 20) + 0x200) & 0x1000); }

}  // namespace

extern "C" void __imp__sub_82B673D0(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_82B673D0) {
  const uint32_t file = ctx.r4.u32;
  __imp__sub_82B673D0(ctx, base);
  const uint32_t table = GuestWord(base, file + 252);
  if (!table || table <= file || table - file > (32u << 20)) return;
  auto inside = [&](uint32_t address) { return address >= file && address < table; };
  std::vector<uint32_t> objects;
  const uint32_t count = GuestWord(base, table + 20), references = GuestWord(base, table + 12);
  for (uint32_t i = 0; i < count && i < 65536; ++i) objects.push_back(GuestWord(base, GuestWord(base, references + i * 4)));
  std::sort(objects.begin(), objects.end());
  // The records, by address: their shader pair and the passes found to draw them.
  struct Record {
    uint32_t vs = 0, ps = 0;
    uint8_t passes = 0;
  };
  std::unordered_map<uint32_t, Record> records;
  for (uint32_t at = file; at + 32 <= table; at += 4) {
    const uint32_t vs_ref = GuestWord(base, at + 8);
    if (!inside(vs_ref)) continue;
    const uint32_t vs = GuestWord(base, vs_ref);
    if (!std::binary_search(objects.begin(), objects.end(), vs)) continue;
    const uint32_t ps_ref = GuestWord(base, at + 12);
    records.emplace(at, Record{vs, inside(ps_ref) ? GuestWord(base, ps_ref) : 0, 0});
  }
  if (records.empty()) return;
  // Which passes draw each record. Passes 1 and 2 run with alpha testing on
  // (pass 1 as alpha to coverage, sub_823A9730 -> sub_823A7F40; pass 2 measured:
  // 42 of the 46 alpha-tested programs left unpredicted were its), and alpha
  // test makes a program of its own in Plume; with the render states each pass
  // is known to draw with, the record's pipelines are predicted whole. Groups
  // are 16 bytes (+8 how many records, +12 the first); a pass table is 8
  // {groups, how many}.
  std::unordered_set<uint32_t> groups;
  for (uint32_t at = file; at + 16 <= table; at += 4) {
    const uint32_t n = GuestWord(base, at + 8);
    if (n && n < 65536 && records.count(GuestWord(base, at + 12))) groups.insert(at);
  }
  for (uint32_t at = file; at + 64 <= table; at += 4) {
    bool valid = true, any = false;
    for (uint32_t pass = 0; pass < 8 && valid; ++pass) {
      const uint32_t first = GuestWord(base, at + pass * 8), n = GuestWord(base, at + pass * 8 + 4);
      if (!n) continue;
      valid = n < 4096 && groups.count(first);
      any = true;
    }
    if (!valid || !any) continue;
    for (uint32_t pass = 0; pass < 8; ++pass) {
      const uint32_t first = GuestWord(base, at + pass * 8), n = GuestWord(base, at + pass * 8 + 4);
      for (uint32_t g = 0; g < n; ++g) {
        const uint32_t group = first + g * 16;
        const uint32_t record_count = GuestWord(base, group + 8), record = GuestWord(base, group + 12);
        for (uint32_t r = 0; r < record_count && r < 65536; ++r) {
          auto found = records.find(record + r * 32);
          if (found != records.end()) found->second.passes |= uint8_t(1u << pass);
        }
      }
    }
  }
  // One pair per shader pair and alpha variant (passes 1 and 2 test alpha); a
  // record no pass table was found for is the plain one.
  constexpr uint8_t kAlphaPasses = 0x06;
  std::map<std::tuple<uint32_t, uint32_t, bool>, uint8_t> pairs;
  for (const auto& [at, record] : records) {
    if (record.passes & kAlphaPasses) pairs[{record.vs, record.ps, true}] |= record.passes & kAlphaPasses;
    if ((record.passes & ~kAlphaPasses) || !record.passes) {
      pairs[{record.vs, record.ps, false}] |= record.passes & ~kAlphaPasses;
    }
  }
  std::vector<renut::PlumeShaderPair> predicted;
  for (const auto& [shaders, passes] : pairs) {
    const auto& [vs, ps, alpha] = shaders;
    renut::PlumeShaderPair pair;
    pair.passes = passes;
    // Vertex shader binding 0, and the pixel shader's microcode (d3d/native/flush.cpp).
    const uint32_t vs_info = GuestWord(base, vs + 112 * 8) + vs;
    pair.vs_address = Physical(GuestWord(base, vs_info + 872) + GuestWord(base, vs + 32));
    pair.vs_dwords = GuestWord(base, vs_info + 876) >> 2;
    if (ps) {
      const uint32_t ps_info = GuestWord(base, ps + 64) + ps;
      pair.ps_address = Physical(GuestWord(base, ps_info + 40) + GuestWord(base, ps + 24));
      pair.ps_dwords = GuestWord(base, ps_info + 44) >> 2;
    }
    pair.alpha_test = alpha && ps;
    if (pair.vs_dwords && pair.vs_dwords < 65536 && pair.ps_dwords < 65536) predicted.push_back(pair);
  }
  if (!predicted.empty()) renut::PlumePredictPipelines(std::move(predicted));
}
