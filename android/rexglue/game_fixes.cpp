// Game-specific overrides of recompiled functions. The generated definitions
// are weak symbols, so a strong definition here replaces them at link time.

#include <atomic>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

// nut_Render_CCalVideoRenderer @ 0x82A0D9E8
//
// Blits a decoded frame of the movie that follows the Rare logo. Video decode
// produces invalid XMEDIA_VIDEO_FRAMEs (garbage width/height/pitch), so this
// path never completes a usable frame: on desktop Linux it SIGSEGVs, here the
// player stops advancing and the last logo frame stays on screen. Skipping the
// blit lets the movie's audio and timing run so the game boots into 3D; movies
// show black until the video frame path is fixed. Same fix as
// etonedemid/reNut_linuxfixes commit 755f620.
REX_EXTERN(sub_82A0D9E8) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    REXLOG_INFO("Renut: skipping intro movie video blit (sub_82A0D9E8)");
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
