// Game-specific overrides of recompiled functions. The generated definitions
// are weak symbols, so a strong definition here replaces them at link time.

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

void renutPostMovieFrame(const std::string& path, uint32_t serial);

REXCVAR_DEFINE_BOOL(renut_scene_msaa, true, "reNut",
                    "Keep the game's MSAA scene surfaces, which the Xbox 360 renders in two "
                    "or four bands (predicated tiling) with every draw sent once per band. "
                    "Off: one pass, no MSAA - the host never resolved the samples anyway");

namespace {

// Movies opened by the game, oldest first, until a renderer takes one. The
// boot sequence opens the MGS logo and the intro together and plays them in
// that order; attract and in-game movies open just before they play.
struct PendingMovie {
  std::string path;
  std::chrono::steady_clock::time_point opened;
};
std::mutex movie_mutex;
std::deque<PendingMovie> pending_movies;

void QueueMovie(std::string path, const char* source) {
  REXLOG_INFO("Renut movie open ({}): {}", source, path);
  std::lock_guard lock(movie_mutex);
  pending_movies.push_back({std::move(path), std::chrono::steady_clock::now()});
}

std::string GuestString(uint8_t* base, uint32_t address, size_t limit) {
  const char* text = reinterpret_cast<const char*>(base + address);
  return address ? std::string(text, strnlen(text, limit)) : std::string();
}

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

extern "C" void __imp__meLoadMovie(PPCContext& __restrict, uint8_t*);
REX_EXTERN(meLoadMovie) {
  REXLOG_INFO("Renut movie request: {}", GuestString(base, ctx.r3.u32, 160));
  __imp__meLoadMovie(ctx, base);
}

// Asset path builder. meLoadMovie formats the boot movies' paths through it;
// other movies take the CCal open below.
extern "C" void __imp__sub_822BE7F0(PPCContext& __restrict, uint8_t*);
REX_EXTERN(sub_822BE7F0) {
  bool movie_call = ctx.lr == 0x822B6A70;
  uint32_t output = ctx.r3.u32;
  __imp__sub_822BE7F0(ctx, base);
  if (movie_call) QueueMovie(GuestString(base, output, 260), "meLoadMovie");
}

// CCal video open @ 0x82A0BE20, reached from sub_8239D6E8 for attract and
// in-game movies (r3 manager, r4 path, r5 where the player is stored).
extern "C" void __imp__sub_82A0BE20(PPCContext& __restrict, uint8_t*);
REX_EXTERN(sub_82A0BE20) {
  std::string path = GuestString(base, ctx.r4.u32, 260);
  __imp__sub_82A0BE20(ctx, base);
  if (int32_t(ctx.r3.u32) >= 0) QueueMovie(std::move(path), "CCal");
}

// nut_Render_CCalVideoRenderer @ 0x82A0D9E8
//
// Blits a decoded frame of the current movie. Video decode produces invalid
// XMEDIA_VIDEO_FRAMEs (garbage width/height/pitch), so this path never
// completes a usable frame: on desktop Linux it SIGSEGVs, here the player
// stops advancing and the last logo frame stays on screen. Skipping the blit
// lets the movie's audio and timing run so the game boots into 3D; the Android
// overlay decodes the original ASF/WMV3 file instead. The original workaround
// came from etonedemid/reNut_linuxfixes commit 755f620.
//
// Each movie gets its own renderer (`this`), so a new `this` takes the oldest
// movie opened. The same address can come back for a later movie: after a
// pause in the blits, a movie opened since the current one started replaces it.
REX_EXTERN(sub_82A0D9E8) {
  using clock = std::chrono::steady_clock;
  static uint32_t current_this = 0;
  static uint32_t serial = 0;
  static std::string current_path;
  static clock::time_point started, last_blit, last_post;
  const uint32_t self = ctx.r3.u32;
  const auto now = clock::now();
  bool changed = false;
  {
    std::lock_guard lock(movie_mutex);
    // Opened but never played (a skipped movie, say): do not hand it on.
    while (!pending_movies.empty() && now - pending_movies.front().opened > std::chrono::seconds(60))
      pending_movies.pop_front();
    bool resumed_later = self == current_this && now - last_blit > std::chrono::seconds(1) &&
                         !pending_movies.empty() && pending_movies.front().opened > started;
    if (self != current_this || resumed_later) {
      changed = true;
      current_this = self;
      started = now;
      ++serial;
      current_path.clear();
      if (!pending_movies.empty()) {
        current_path = std::move(pending_movies.front().path);
        pending_movies.pop_front();
      }
    }
  }
  last_blit = now;
  if (changed) {
    REXLOG_INFO("Renut movie #{}: renderer {:08X} shows {}", serial, self,
                current_path.empty() ? "(unknown)" : current_path);
  }
  if (!current_path.empty() && (changed || now - last_post > std::chrono::milliseconds(500))) {
    last_post = now;
    renutPostMovieFrame(current_path, serial);
  }
  ctx.r3.u64 = 0;
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
