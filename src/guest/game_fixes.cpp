// Game-specific overrides of recompiled functions. The generated definitions
// are weak symbols, so a strong definition here replaces them at link time.

#include "guest_address.h"
#include "platform/log.h"
#include "platform/settings.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <string>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdint>
#include <cstring>

#include <rex/kernel/object/kernel_thread.h>
#include <rex/ppc/context.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/ppc/hook.h>

#include "d3d/frontend/native_d3d.h"
#include "plume/frame_pacing.h"
#include "plume/plume_gpu.h"





namespace renut::pacing {
void ApplyConfiguredTarget() {
  int fps = renut::GetSettings().fps_target.Get();
  SetTarget(fps);
  PaceOriginalCell().store(renut::GetSettings().pace_original_fps.Get(), std::memory_order_relaxed);
}
}  // namespace renut::pacing

namespace {

// The game's MSAA mode (0 none, 1 2x, 2 4x), read by the scene surface setup
// (sub_823ED2A8), the tile count (sub_8222A990) and the surface chooser (sub_823ED260).
constexpr uint32_t kSceneMsaaMode = 0x82F9EC18;
// The scene's surfaces (sub_823ED2A8): colour and depth with the mode's samples, then the single-sample
// pair the post-processing draws into.
constexpr uint32_t kSceneColorMsaa = 0x82F9EB38, kSceneDepthMsaa = 0x82F9EB44;

// The mode is the player's choice, whatever the game's own setting holds (its default is 2x).
uint32_t WantedMsaaMode() { return uint32_t(std::clamp(renut::GetSettings().msaa.Get(), 0, 2)); }

// The mode the game is in: what the hooks below go by. The option changes on another thread at any moment; the
// game's word only in ApplySceneMsaa, together with the surfaces.
uint32_t SceneMsaaMode(uint8_t* base) {
  uint32_t mode;
  std::memcpy(&mode, renut::GuestPtr(base, kSceneMsaaMode), sizeof(mode));
  return std::min(__builtin_bswap32(mode), 2u);
}

// The MSAA type (bits 16-17 of a D3DSurface's +0x18, its RB_SURFACE_INFO) of the surface a pointer names.
void SetSurfaceMsaaType(uint8_t* base, uint32_t pointer_va, uint32_t type) {
  uint32_t surface;
  std::memcpy(&surface, renut::GuestPtr(base, pointer_va), sizeof(surface));
  surface = __builtin_bswap32(surface);
  if (!surface) return;
  uint8_t* word = renut::GuestPtr(base, surface + 0x18);
  uint32_t info;
  std::memcpy(&info, word, sizeof(info));
  info = (__builtin_bswap32(info) & ~(3u << 16)) | (type << 16);
  info = __builtin_bswap32(info);
  std::memcpy(word, &info, sizeof(info));
}

// Puts the game in the player's mode. The game sizes its scene surfaces from the mode only when it makes its
// device (sub_823ED2A8, which also makes the device again and is not something to run in the middle of a game),
// so a change made while it runs is given to the two MSAA surfaces it already has: with one band they are the
// screen's size in every mode, and the type in their header is all that differs (D3D::SetSurfaceHeader: the
// pitch words of a 1280 wide surface are the same for 2x and 4x). The host makes their images again from the
// header the next time they are bound (Targets::SurfaceTarget keys them by it).
void ApplySceneMsaa(uint8_t* base) {
  auto* mode = reinterpret_cast<uint32_t*>(renut::GuestPtr(base, kSceneMsaaMode));
  const uint32_t wanted_mode = WantedMsaaMode();
  const uint32_t wanted = __builtin_bswap32(wanted_mode);
  if (*mode != wanted) {
    static std::atomic<uint32_t> logged{0};
    if (logged.fetch_add(1) < 8) {
      REXLOG_CAT_INFO(rex::log::renut, "Renut: scene MSAA mode {} -> {}: one band, no predicated tiling", __builtin_bswap32(*mode),
                  wanted_mode);
    }
    // The type the game gives the mode (sub_823ED260): 2x is 4x when the word at 0x82F9EBE4 is 0.
    uint32_t hidef;
    std::memcpy(&hidef, renut::GuestPtr(base, 0x82F9EBE4), sizeof(hidef));
    const uint32_t type = wanted_mode == 0 ? 0u : wanted_mode == 2 ? 2u : hidef ? 1u : 2u;
    SetSurfaceMsaaType(base, kSceneColorMsaa, type);
    SetSurfaceMsaaType(base, kSceneDepthMsaa, type);
    *mode = wanted;
  }
}

}  // namespace

// The game selects a presentation interval through this routine. Index 0 is its
// immediate mode; preserve the original routine's device bookkeeping. Anything
// but the game's own 30 FPS asks for the immediate one here, as before the
// frame-rate slider; a target that changes later is also applied per frame at the
// swap's synchronization point (d3d/frontend/sync.cpp SynchronizeToPresentationInterval).
// What the game ends up with is noted, so that the command thread paces the frames
// itself whenever the vblank does not (frame_pacing.h VblankPaces).
extern "C" void __imp__rex_setParamInterval(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(rex_setParamInterval) {
  if (renut::pacing::ImmediatePresentation() && ctx.r3.u32 != 0) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
      REXLOG_CAT_INFO(rex::log::renut, "Renut: presentation interval {} -> immediate (frame-rate target {})", ctx.r3.u32,
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
// The other half of the same defect, one step earlier in the same function: meInternalFree (the game's
// free) looks the block's owner up (sub_82240178 gives its index in the table of heaps at 0x82F84BC8,
// 56 bytes an entry) and, when that entry has no heap (the word at +60), carries on with a null entry and
// reads it at guest address 0x24. A free of a block no heap owns releases nothing, as above.
// (The double frees seen under RexGlue 0.11 came from the race on the global pools, fixed below.)
//
// Every free of the game comes through here, and the original looks the owner up itself, so the check must not
// cost a second lookup. sub_82240178 walks the five entries of the table and, for each one with a heap, asks
// sub_82240200 for the block's size; that answer comes from the block's header alone (the heap only decides
// whether a debug check runs first), and the entry owns the block when its tag (the word at +56) is the byte at
// block + size - 2. The same reads are done here; only a block they find no owner for goes through the
// recompiled lookup, which then decides.
namespace {
uint32_t GuestU16(const uint8_t* base, uint32_t address) {
  return uint32_t(base[address]) << 8 | base[uint32_t(address + 1)];
}
uint32_t GuestU32(const uint8_t* base, uint32_t address) {
  return GuestU16(base, address) << 16 | GuestU16(base, address + 2);
}
bool BlockHasOwner(const uint8_t* base, uint32_t block) {
  constexpr uint32_t kHeapTable = 0x82F84BC8, kEntrySize = 56, kEntries = 5;
  if (block < 0x1000) return false;
  const uint8_t flags = base[block - 11];
  uint32_t tag = 255;
  if (flags & 1) {
    const uint32_t size = (flags & 8) ? GuestU32(base, block - 24) - GuestU16(base, block - 16) - 48
                                      : (GuestU16(base, block - 16) << 4) - base[block - 10];
    if (size != 0xFFFFFFFFu) tag = base[uint32_t(block + size - 2)];
  }
  for (uint32_t i = 0; i < kEntries; ++i) {
    const uint32_t entry = kHeapTable + i * kEntrySize;
    if (GuestU32(base, entry + 60) && GuestU32(base, entry + 56) == tag) return true;
  }
  return false;
}
}  // namespace

extern "C" void __imp__meInternalFree(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82240178(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(meInternalFree) {
  if (BlockHasOwner(base, ctx.r3.u32)) {
    __imp__meInternalFree(ctx, base);
    return;
  }
  constexpr uint32_t kHeapTable = 0x82F84BC8, kEntrySize = 56, kImageEnd = 0x830A0000;
  PPCContext probe = ctx;
  __imp__sub_82240178(probe, base);
  const uint32_t index = probe.r3.u32;
  const uint32_t entry = kHeapTable + index * kEntrySize;
  uint32_t heap = 0;
  if (entry >= kHeapTable && entry + 64 <= kImageEnd) {  // the table's size is not known: anywhere in the image
    const uint8_t* word = base + entry + 60;
    heap = uint32_t(word[0]) << 24 | uint32_t(word[1]) << 16 | uint32_t(word[2]) << 8 | word[3];
  }
  if (!heap) {
    static std::atomic<uint32_t> reported{0};
    if (reported.fetch_add(1) < 8) {
      REXLOG_CAT_WARN(rex::log::renut, "Renut: free of a block no heap owns (block {:08X}, owner {}, caller {:08X}) skipped",
                      ctx.r3.u32, index, uint32_t(ctx.lr));
      // The bytes around the block: the owner is found by the tag byte at block-2, so what is there tells a
      // stomped header from a pointer the heap never gave out. (Diagnostic.)
      const uint32_t block = ctx.r3.u32;
      if (block >= 0x40000100 && block < 0x80000000) {
        std::string dump;
        for (uint32_t i = block - 48; i < block + 16; ++i) dump += std::format("{:02X}{}", base[i], (i - (block - 48)) % 16 == 15 ? " |" : "");
        REXLOG_CAT_WARN(rex::log::renut, "Renut:   bytes {:08X}..{:08X}: {}", block - 48, block + 16, dump);
      }
    }
    ctx.r3.u32 = 0;
    return;
  }
  __imp__meInternalFree(ctx, base);
}

extern "C" void __imp__sub_8223FD48(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_8223FD48) {
  if (ctx.r3.u32 == 0) {
    static std::atomic<uint32_t> reported{0};
    if (reported.fetch_add(1) < 8)
      REXLOG_CAT_WARN(rex::log::renut, "Renut: heap free without an owning heap (block {:08X}) skipped", ctx.r5.u32);
    ctx.r3.u32 = 1;
    return;
  }
  __imp__sub_8223FD48(ctx, base);
}

// Photo Mode's capture routine @ 0x82535F20, BOOL (D3DTexture *photo): the shot, and the pause
// menu's backdrop. It resolves the screen into a scratch texture, waits for the copy and untiles
// that texture's memory into the photo's with the CPU (XGCopySurface); the game then encodes its
// JPEG from those texels. Resolves stay on the GPU here, so that read saw nothing: a black
// picture. Done as reNut PC does it: the render target is read back into the photo's rows by the
// GPU backend (native_d3d::CapturePhoto), with no scratch texture and no resolve. A routine that is
// never called costs nothing while playing.
REX_HOOK_RAW(sub_82535F20) {
  renut::native_d3d::CapturePhoto(ctx, base);
}

// Scene changes: the game's own loading screen stays up until the new scene's
// pipelines exist, as UnleashedRecomp holds its loading screen
// (CGameModeStage::ExitLoading) until its compiles finish, so a scene is never
// drawn without them. The request only moves the scene's pipelines to the front
// of the queue (gpu/core/host.cpp: PlumeSceneRequested); the hold is further down,
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
// A scene was asked for and has not been opened yet (set by sub_822BC0D8, cleared where the scene starts).
std::atomic<bool> g_scene_loading{false};
// The game's main thread, as the scene hooks see it; the pump draws, so only this thread runs it.
thread_local bool t_game_main = false;
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
  t_game_main = true;
  g_scene_loading.store(true, std::memory_order_relaxed);
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
  t_game_main = true;
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
      g_scene_loading.store(false, std::memory_order_relaxed);
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
      g_scene_loading.store(false, std::memory_order_relaxed);
    pipeline_hold = false;
  } else if (GuestWord(base, scene + 16) == 2 && !pipeline_hold) {
    pipeline_hold = true;
    REXLOG_CAT_INFO(rex::log::renut, "Renut: scene {:08X} loaded; its loading screen held for its pipelines", scene);
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
// The game's stall pump, sub_8223C650 (no arguments): what the loader calls while it waits (an asset open, an
// allocation, a lock: meInternalAlloc, assetManOpenParams, assetDbBundleLoadBundle through sub_82235E68) to keep the
// loading screen going. At most every 33 ms it updates and draws one frame of the
// loading screen and swaps; it returns at once when no loading screen is up, or while it is itself running
// (dword_82FAAFFC). Two places here run it: the hold below, and the game's own wait (sub_821E04F0, the
// WaitForSingleObject of the engine's job system) while a scene loads: that wait does not pump, and a world build
// that sits in it (the main thread's tick, 22 s without one frame on the S23) left the loading screen standing still.
extern "C" REX_FUNC(sub_8223C650);
extern "C" void __imp__sub_821E04F0(PPCContext& __restrict, uint8_t*);



namespace {

thread_local bool t_in_pump = false;
thread_local std::chrono::steady_clock::time_point t_last_pump;
constexpr uint32_t kPumpStopFlag = 0x82FAB13A;  // set: the pump never returns (a swap loop)

// One run of the game's stall pump on this thread, with the caller's registers kept.
void RunPump(PPCContext& ctx, uint8_t* base) {
  if (t_in_pump || *renut::GuestPtr(base, kPumpStopFlag) != 0) return;
  const PPCContext saved = ctx;
  t_last_pump = std::chrono::steady_clock::now();
  t_in_pump = true;
  sub_8223C650(ctx, base);
  t_in_pump = false;
  ctx = saved;
}

}  // namespace

// WaitForSingleObject(handle r3, timeout in ms r4 (-1: none), alertable r5) -> status r3 (0x102: timed out).
REX_HOOK_RAW(sub_821E04F0) {
  const uint32_t timeout = ctx.r4.u32;
  if (!t_game_main || t_in_pump || timeout == 0 || !g_scene_loading.load(std::memory_order_relaxed) ||
      !renut::GetSettings().pump_loading_waits.Get()) {
    __imp__sub_821E04F0(ctx, base);
    return;
  }
  const PPCContext entry = ctx;
  const auto start = std::chrono::steady_clock::now();
  for (;;) {
    const uint64_t elapsed =
        uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    uint32_t slice = 16;
    if (timeout != 0xFFFFFFFFu) {
      if (elapsed >= timeout) {
        ctx = entry;
        ctx.r4.u32 = 0;
        __imp__sub_821E04F0(ctx, base);  // a last look, which also leaves the registers as the original does
        return;
      }
      slice = uint32_t(std::min<uint64_t>(slice, timeout - elapsed));
    }
    ctx = entry;
    ctx.r4.u32 = slice;
    __imp__sub_821E04F0(ctx, base);
    if (ctx.r3.u32 != 0x102) {
      // Signalled (or failed): as the original, but a wait that comes back at once is no reason to skip the frame the
      // pump owes (it keeps its own 33 ms between frames; this only spares the calls).
      if (std::chrono::steady_clock::now() - t_last_pump >= std::chrono::milliseconds(20)) RunPump(ctx, base);
      return;
    }
    RunPump(ctx, base);
  }
}

extern "C" void __imp__sub_822BBE58(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_822BBE58) {
  if (in_scene_update && ctx.r4.u32 != 0 && scene_asked_since_boot.load()) {
    const auto start = std::chrono::steady_clock::now();
    uint32_t polls = 0;
    const bool pump = renut::GetSettings().pump_loading_hold.Get();
    while (!renut::PlumeSceneReady()) {
      // A raw hook is not checked for a stop request, and the runtime's ForceStop only reaches a thread
      // inside recompiled code: without this, closing the game during the hold waited up to 60 s.
      rex::kernel::KernelThread::CheckStop();
      ++polls;
      if (pump) RunPump(ctx, base);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (polls) {
      REXLOG_CAT_INFO(rex::log::renut, "Renut: scene start held its loading screen {} ms more for the pipelines of the models it loaded",
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
  ApplySceneMsaa(base);
  __imp__sub_823ED2A8(ctx, base);
}

// Tile count @ 0x8222A990, asked each frame by the tiling code: always one band, the screen's size, with
// whatever samples the mode gives its surfaces (the console needs 2 or 4 bands for 2x or 4x because its 10 MB
// of EDRAM cannot hold them; the host has no EDRAM). With the count at 1 the game sizes the MSAA surfaces to
// the whole screen, picks them in its scene begin (sub_8227B808) and leaves the tile table alone.
REX_HOOK_RAW(sub_8222A990) {
  ApplySceneMsaa(base);
  ctx.r3.u64 = 1;
}

// The surface chooser @ 0x823ED260 (0 none, else the MSAA type of the scene's surfaces).
extern "C" void __imp__sub_823ED260(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_823ED260) {
  ApplySceneMsaa(base);
  __imp__sub_823ED260(ctx, base);
}

// The scene's end @ 0x8227A9F0 (IDB sub_8227D5F0). With a tile count above 1 it resolves the scene, ends the tiling and then puts the
// game's own pair back with SetRenderTarget / SetDepthStencilSurface (0x82F9EB40 / EB48, the single-sample pair the bloom
// composite and the interface are drawn into, and the picture that is shown comes from). With one tile it only resolves, and
// leaves bound whatever the scene used: the MSAA pair, so that everything after the scene drew into the surface the
// composite is not in. The same restore is done here.
extern "C" void __imp__sub_8227A9F0(PPCContext& __restrict, uint8_t*);
extern "C" void sub_8227AB68(PPCContext& __restrict, uint8_t*);  // D3DDevice_SetRenderTarget
extern "C" void sub_8227AF40(PPCContext& __restrict, uint8_t*);  // D3DDevice_SetDepthStencilSurface
constexpr uint32_t kPostColor = 0x82F9EB40, kPostDepth = 0x82F9EB48, kDevicePointer = 0x82FAC7C0;
REX_HOOK_RAW(sub_8227A9F0) {
  const uint32_t sp = ctx.r1.u32;
  __imp__sub_8227A9F0(ctx, base);
  if (!SceneMsaaMode(base)) return;
  const uint32_t device = GuestWord(base, kDevicePointer);
  const uint32_t color = GuestWord(base, kPostColor), depth = GuestWord(base, kPostDepth);
  if (!device || !color || !depth) return;
  PPCContext call = ctx;
  call.r1.u32 = sp;
  call.r3.u32 = device;
  call.r4.u32 = 0;
  call.r5.u32 = color;
  sub_8227AB68(call, base);
  call = ctx;
  call.r1.u32 = sp;
  call.r3.u32 = device;
  call.r4.u32 = depth;
  sub_8227AF40(call, base);
  static std::atomic<uint32_t> logged{0};
  if (logged.fetch_add(1) < 3) REXLOG_CAT_INFO(rex::log::renut, "Renut: scene ended; the device back on the single-sample pair {:08X} / {:08X}", color, depth);
}

// The pass begin @ 0x823DBEB8 (r3 the job, r4 the device, r5 no_tiling, r6 use_table), where a scene job's
// command buffer starts: with a tile count of 1 it binds the single-sample pair (0x82F9EB40 / EB48) whatever
// the mode, which would draw the scene without the samples. After it, the MSAA pair takes their place, as the
// scene begin (sub_8227B808) does for the direct path. The jobs that ask for no tiling (post-processing) stay
// on the single-sample pair.
extern "C" void __imp__sub_823DBEB8(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_823DBEB8) {
  const uint32_t device = ctx.r4.u32, no_tiling = ctx.r5.u32;
  const uint32_t sp = ctx.r1.u32;
  __imp__sub_823DBEB8(ctx, base);
  if (no_tiling || !SceneMsaaMode(base)) return;
  const uint32_t color = GuestWord(base, kSceneColorMsaa), depth = GuestWord(base, kSceneDepthMsaa);
  if (!device || !color || !depth) return;
  PPCContext call = ctx;
  call.r1.u32 = sp;
  call.r3.u32 = device;
  call.r4.u32 = 0;
  call.r5.u32 = color;
  sub_8227AB68(call, base);
  call = ctx;
  call.r1.u32 = sp;
  call.r3.u32 = device;
  call.r4.u32 = depth;
  sub_8227AF40(call, base);
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) REXLOG_CAT_INFO(rex::log::renut, "Renut: scene jobs bound to the MSAA surfaces {:08X} / {:08X}", color, depth);
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
REX_HOOK_RAW(sub_823B18C8) {
  {
    const uint32_t scene = GuestWord(base, ctx.r3.u32 + 40);
    const uint32_t split_flag = scene ? GuestWord(base, scene + 44) : 0;
    const uint32_t view = GuestWord(base, ctx.r3.u32 + 52);
    if (scene && split_flag == 1) {
      // The view's matrices: +128 whole screen, +192 upper band, +256 lower.
      uint8_t* block = renut::GuestPtr(base, scene + (view << 9));
      std::memcpy(block + 192, block + 128, 64);
      std::memcpy(block + 256, block + 128, 64);
      static std::atomic<bool> logged{false};
      if (!logged.exchange(true)) REXLOG_CAT_INFO(rex::log::renut, "Renut: scene {:08X} band jobs given the whole-screen projection", scene);
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
    REXLOG_CAT_WARN(rex::log::renut, "Renut: GPU fence wait exceeded the D3D hang timeout ({}x); still waiting", n);
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

// Direct3D's view of physical memory (d3d/frontend/internal.h's Phys).
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
    // Vertex shader binding 0, and the pixel shader's microcode (d3d/frontend/flush.cpp).
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

// The title's audio engine init (sub_82664680, IDB 0x82667280) writes the headers of its static events in place
// before it starts its mixer threads: 0x82FA7C3C (the render callback asks for a frame), 0x82FA7C18 (the mixer
// has made it) and 0x82FA7C5C (the title ends), each with its signal state 0. The game makes a new engine when it
// loads its first 3D scene. The old engine's leader thread (sub_82664508), told to stop, sets 0x82FA7C18 once
// more as it leaves, with nobody waiting on it; on the console the new engine's header write clears that signal.
// The SDK keeps an event's state in the host object behind the address and never reads the header again, so the
// new engine's first callback found 0x82FA7C18 already set, returned at once and cleared its mark at engine+0x12C;
// the new leader, woken by that callback's request, then found 0x12C clear, took it as its order to stop and
// left after 4 ms. Nothing set 0x82FA7C18 again: the next callback waited for ever and the sound died. The events
// are reset through the kernel before the init runs, as the header write does on the console. Only an event the
// title has initialised before (its wait list points at itself) is touched: on the first init the headers are
// still zero, and a kernel call then would make the SDK build the event from a type-0 header, as a manual-reset
// one.
extern "C" void __imp__KeResetEvent(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82664680(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_82664680) {
  for (uint32_t event : {0x82FA7C3Cu, 0x82FA7C18u, 0x82FA7C5Cu}) {
    uint32_t wait_list;  // X_DISPATCH_HEADER::wait_list_flink, at +8
    std::memcpy(&wait_list, renut::GuestPtr(base, event + 8), sizeof(wait_list));
    if (__builtin_bswap32(wait_list) != event + 8) continue;
    PPCContext call = ctx;
    call.r3.u64 = event;
    __imp__KeResetEvent(call, base);
  }
  __imp__sub_82664680(ctx, base);
}

// The game's global pools at 0x82FAC600 and 0x82FAC604. Their getter (sub_82364A20, IDB 0x82367620) makes them
// when 0x82FAC600 is 0, through sub_82364A88 (IDB 0x82367688), which destroys both pools (sub_82364BB8, IDB
// 0x823677B8), zeroes the pointers and makes new ones; a scene change calls sub_82364A88 too, and a command of the
// renderer destroys them in place (IDB 0x823D3594). None of it takes a lock. On the console the main thread and
// the task worker that also asks for the pool do not run at the same instant (one hardware thread, one quantum);
// here they do: the main thread zeroed 0x82FAC600 while remaking the pools, a worker's getter saw 0 and remade
// them as well, and both destroyed the same pool and its buffer (the double free of block 438E1910 and its
// owner, then a cyclic free list or a call through a dead object's table). The getter, the remake and the
// destroy run under one lock, so a getter that comes in during a remake waits for it and takes the new pool.
//
// The getter is called all the time and the pools nearly always exist, so it takes the lock only when it has to:
// it returns 0x82FAC604 when the low byte of r3 is set and 0x82FAC600 otherwise, and that is answered here
// while both pools exist and no remake or destroy is under way (looked at before and after reading them).
// Anything else goes through the lock to the original, as before.
namespace {
std::recursive_mutex g_pool_mutex;
std::atomic<uint32_t> g_pool_changing{0};
constexpr uint32_t kPoolA = 0x82FAC600, kPoolB = 0x82FAC604;
uint32_t LoadPool(const uint8_t* base, uint32_t address) {
  return __builtin_bswap32(__atomic_load_n(reinterpret_cast<const uint32_t*>(base + address), __ATOMIC_ACQUIRE));
}
// Held by the remake and the destroy: the lock, and the mark the getter's fast path looks at.
struct PoolChange {
  std::lock_guard<std::recursive_mutex> lock{g_pool_mutex};
  PoolChange() { g_pool_changing.fetch_add(1); }
  ~PoolChange() { g_pool_changing.fetch_sub(1); }
};
}  // namespace
extern "C" void __imp__sub_82364A20(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82364A88(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82364BB8(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_82364A20) {
  if (g_pool_changing.load() == 0) {
    const uint32_t a = LoadPool(base, kPoolA), b = LoadPool(base, kPoolB);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (a && b && g_pool_changing.load() == 0) {
      ctx.r3.u64 = (ctx.r3.u32 & 0xFF) ? b : a;
      return;
    }
  }
  std::lock_guard lock(g_pool_mutex);
  __imp__sub_82364A20(ctx, base);
}
REX_HOOK_RAW(sub_82364A88) {
  PoolChange change;
  __imp__sub_82364A88(ctx, base);
}
REX_HOOK_RAW(sub_82364BB8) {
  PoolChange change;
  __imp__sub_82364BB8(ctx, base);
}
