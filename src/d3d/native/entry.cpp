// The public entry points (native_d3d.h).
#include "internal.h"

namespace renut::native_d3d {

// ---------------------------------------------------------------------------
// Public entry points.

Mode GetMode() { return Mode::kNative; }

bool OnEnter(uint32_t function, PPCContext& ctx, uint8_t* base) {
  using namespace renut::d3d;
  if (function < g_calls.size() && function != renut::d3d::kD3D_CBlocker_Check) {
    g_calls[function].fetch_add(1, std::memory_order_relaxed);
  }
  t_function = function;
  switch (Function(function)) {
    case kD3DDevice_DrawIndexedVertices:
      return DrawIndexedVertices(ctx, base);
    case kD3DDevice_DrawVertices:
      return DrawVertices(ctx, base);
    case kD3DDevice_BeginVertices:
      return BeginVertices(ctx, base);
    case kD3DDevice_BeginIndexedVertices:
      return BeginIndexedVertices(ctx, base);
    case kD3DDevice_DrawIndexedTessellatedVertices:
      return DrawIndexedTessellatedVertices(ctx, base);
    case kD3D_ClearF:
      return ClearNative(ctx, base);
    case kD3DDevice_Resolve:
      return ResolveNative(ctx, base);
    // Memory export and GPR allocation change nothing the lists draw with
    // (the export address is a coherency base; the dummy draws drain the
    // console's pipeline): their PM4 goes to segments nobody walks.
    case kD3DDevice_BeginCommandBuffer:
      BeginCommandBuffer(ctx);
      return false;
    case kD3DDevice_EndCommandBuffer:
      EndCommandBuffer(ctx, base);
      return false;
    case kD3DDevice_RunCommandBuffer:
      RunCommandBuffer(ctx, base);
      return false;
    case kD3DDevice_SetPredication:
      SetPredication(ctx);
      return false;
    case kD3DDevice_SetCommandBufferPredication:
      SetCommandBufferPredication(ctx);
      return false;
    case kD3D_SetSurfaceClip:
      SetSurfaceClip(ctx, base);
      return false;
    case kD3DDevice_Swap:
      Swap(ctx);
      return false;
    case kD3D_CDevice_KickOff:
    case kD3D_CDevice_KickOffSegment: {
      std::lock_guard lock(g_front.mutex);
      Publish();
      return false;
    }
    case kD3D_CDevice_SetFence:
      SetFence(ctx, base);
      return false;
    case kD3D_CDevice_AddCallsToPrimaryBuffer:
      return AddCallsToPrimaryBuffer(ctx, base);
    case kD3D_InsertCallback:
      return InsertCallback(ctx, base);
    case kD3D_SynchronizeToPresentationInterval:
      return SynchronizeToPresentationInterval(ctx, base);
    case kD3D_CBlocker_Check:
      CheckWait(ctx, base);
      return false;
    default:
      return false;
  }
}

// D3DDevice_SetRingBufferParameters(device, ...) (sub_82657508), the device's first act: it
// writes the first segment, whose type-0 packets set up the command processor. The one the CPU
// depends on is the scratch register writeback: SCRATCH_ADDR names the block (device + 10900,
// which SynchronizeToPresentationInterval waits on at +4) and SCRATCH_UMSK (0x20037) which
// registers are written back to it. Every later scratch write the lists carry (callbacks, the
// present interval) lands where this says, so it is a record of its own, ahead of the first
// doorbell, whether or not that segment is walked.
void RingBufferParameters(PPCContext& ctx, uint32_t dev, uint8_t* base) {
  if (ctx.r3.u32 != 0 || !dev) return;  // not S_OK
  const uint32_t block = U32(base, dev + 10900);
  if (!block) return;
  std::lock_guard lock(g_front.mutex);
  PushEffects(Current(dev), {{kRegScratchAddr, Phys(block)}, {kRegScratchUmsk, 0x00020037u}});
  // The only device that sets a ring up is the one that draws and swaps, so from here its
  // doorbells are the main ones; and the lists carry their sync records from the first doorbell.
  if (!g_front.main_device) g_front.main_device = dev;
  if (!g_front.sync_recording) {
    g_front.sync_recording = true;
    REXLOG_INFO("Renut: native sync on from the device's first act (swap {})", g_front.swaps_seen);
  }
}

void OnLeave(uint32_t function, uint32_t r3, PPCContext& ctx, uint8_t* base) {
  if (function == renut::d3d::kD3DDevice_SetRingBufferParameters) RingBufferParameters(ctx, r3, base);
}

void TakePublished(std::vector<std::shared_ptr<const List>>& out) {
  std::lock_guard lock(g_front.published_mutex);
  while (!g_front.published.empty()) {
    out.push_back(std::move(g_front.published.front()));
    g_front.published.pop_front();
  }
}

// ---------------------------------------------------------------------------

}  // namespace renut::native_d3d
