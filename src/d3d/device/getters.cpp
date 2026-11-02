// Layer 1c: the getters, resource reference counting and the last device-state
// calls. Ported from reNut's hooks/state.cpp, hooks/resource.cpp and
// hooks/device.cpp (masterspike52's reNut); same behaviour, guest memory only.
// Offsets in the CDevice-private area go through Layout() (device.h).
#include "d3d/device/getters.h"

#include <atomic>
#include <cstring>

#include "d3d/device/internal.h"

namespace renut::d3d::port {

namespace {

// Fields GetViewport, BeginIndexedVertices, AddRef and GetDisplayMode use.
constexpr u32 kViewportMinZ = Layout(0x3180);
constexpr u32 kViewportMaxZ = Layout(0x3184);
constexpr u32 kIndexedVerticesRing = 0x3484;  // the ring pointer BeginIndexedVertices saved
constexpr u32 kRing = 0x30;                   // m_pRing
// GetDisplayMode: width, height, refresh rate, format.
constexpr u32 kDisplayWidth = 0x34E8, kDisplayHeight = 0x34EC, kDisplayRate = 0x5434,
              kDisplayFormat = 0x34F0;

u8* Guest(Device device, u32 address) { return renut::GuestPtr(device.base, address); }

u32 LoadU32(Device device, u32 address) { return address ? LoadBE32(Guest(device, address)) : 0; }

void StoreU32(Device device, u32 address, u32 value) {
  if (address)
    StoreBE32(Guest(device, address), value);
}

// lwarx/stwcx. add on a guest word; returns the old value.
u32 AtomicAddGuest(Device device, u32 address, u32 delta) {
  auto* word_bytes = reinterpret_cast<u32*>(Guest(device, address));
  std::atomic_ref<u32> word(*word_bytes);
  u32 raw = word.load(std::memory_order_relaxed);
  for (;;) {
    const u32 old = __builtin_bswap32(raw);
    if (word.compare_exchange_weak(raw, __builtin_bswap32(old + delta), std::memory_order_acq_rel))
      return old;
  }
}

bool IsTextureLevel(u32 common) {
  return (common & 0xF) == 4 && (common & kCommonTextureLevel);
}

}  // namespace

// ULONG D3DResource_AddRef(D3DResource*) [0x822088B0]
u32 AddRefResource(Device device, u32 resource) {
  if (!device || !resource)
    return 0;
  const u32 count = AtomicAddGuest(device, resource + 4, 1) + 1;
  if (count == 1 && IsTextureLevel(LoadU32(device, resource)))
    AddRefResource(device, LoadU32(device, resource + 0x18));
  return count;
}

// ULONG D3DResource_Release(D3DResource*) [0x82208720]
u32 ReleaseResource(Device device, u32 resource, const ResourceDestroyer& destroyer) {
  if (!device || !resource)
    return 0;
  const u32 count = AtomicAddGuest(device, resource + 4, ~0u) - 1;
  if (count)
    return count;
  if (IsTextureLevel(LoadU32(device, resource)))
    ReleaseResource(device, LoadU32(device, resource + 0x18), destroyer);
  if (destroyer.destroy)
    destroyer.destroy(destroyer.user, resource);
  return 0;
}

// D3DRESOURCETYPE D3DResource_GetType(D3DResource*) [0x82208110]
u32 ResourceType(Device device, u32 resource) {
  const u32 common = LoadU32(device, resource);
  const u32 type = common & 0xF;
  if (type == 3) {
    switch (Extract(LoadU32(device, resource + 0x30), 9, 2)) {
      case 3: return 0x12;
      case 2: return 0x11;
      case 0: return 0x14;
      default: return (LoadU32(device, resource + 0x20) & 0x400) ? 0x13 : 3;
    }
  }
  if (type == 4 && (common & kCommonTextureLevel) &&
      (LoadU32(device, LoadU32(device, resource + 0x18) + 0x30) & 0x600) == 0x400)
    return 0x10;
  return type;
}

// UINT D3DBaseTexture_GetLevelCount(D3DBaseTexture*) [0x821EF890]
u32 BaseTextureLevelCount(Device device, u32 texture) {
  return Extract(LoadU32(device, texture + 0x2C), 6, 4) + 1;
}

// UINT D3DArrayTexture_GetArraySize(D3DArrayTexture*) [0x82655DA0]
u32 ArrayTextureSize(Device device, u32 texture) {
  return (LoadU32(device, texture + 0x24) >> 26) + 1;
}

// D3DSurface *GetRenderTarget(D3DDevice*, DWORD Index) [0x8264D8D0]
u32 GetRenderTarget(Device device, u32 index) {
  const u32 surface = device && index < 4 ? device.W(off::RenderTargets + index * 4) : 0;
  if (surface)
    AddRefResource(device, surface);
  return surface;
}

// D3DSurface *GetDepthStencilSurface(D3DDevice*) [0x8264D910]
u32 GetDepthStencilSurface(Device device) {
  const u32 surface = device ? device.W(off::DepthStencil) : 0;
  if (surface)
    AddRefResource(device, surface);
  return surface;
}

// D3DVertexBuffer *GetStreamSource(D3DDevice*, UINT StreamNumber, UINT *pOffsetInBytes,
//     UINT *pStride) [0x8264D840]: the offset is the stream's fetch constant address
// less the buffer's.
u32 GetStreamSource(Device device, u32 stream, u32 offset_va, u32 stride_va) {
  if (!device || stream >= 16)
    return 0;
  const u32 buffer = device.W(off::StreamSources + stream * 4);
  u32 bytes = 0;
  if (buffer) {
    const u32 fetch = device.W(off::TextureFetch + (95 - stream) * 8);
    bytes = fetch - GpuAddress(LoadU32(device, buffer + 24));
    AddRefResource(device, buffer);
  }
  StoreU32(device, offset_va, bytes);
  StoreU32(device, stride_va, u32(device.B(off::StreamStrides + stream)) << 2);
  return buffer;
}

// void GetViewport(D3DDevice*, D3DVIEWPORT9*) [0x82276AC0]
void GetViewport(Device device, u32 viewport_va) {
  if (!device || !viewport_va)
    return;
  u8* viewport = Guest(device, viewport_va);
  for (u32 i = 0; i < 4; ++i)  // fctidz, the low word
    StoreBE32(viewport + i * 4, u32(TruncateToI64(device.F(kViewport + i * 4))));
  StoreBE32(viewport + 16, device.W(kViewportMinZ));
  StoreBE32(viewport + 20, device.W(kViewportMaxZ));
}

// void D3DDevice_SetVertexFetchConstant(D3DDevice*, UINT VertexFetchRegister,
//     D3DVertexBuffer*, UINT Offset) [0x8264DB38]: a vertex fetch slot of the 3 in a
// fetch constant, made a vertex fetch constant first.
void SetVertexFetchConstant(Device d, u32 reg, u32 vertex_buffer, u32 offset) {
  if (!d || !vertex_buffer || reg > 0x5F)
    return;
  const u8* vb = Guest(d, vertex_buffer);
  const u32 slot = (0x5F - reg) / 3;
  const u32 part = (0x5F - reg) - slot * 3;
  const u32 constant = off::TextureFetch + slot * 24;
  const u32 type = d.W(constant) & 3;
  if (type == 0 || type == 2)
    std::memset(d.bytes + constant, 0, 24);
  d.SetW(constant, Insert(d.W(constant), 1, 0, 2));
  d.Dirty(kPendingFetch, u64(1) << (31 - slot));
  d.SetW(constant + part * 8, GpuAddress(LoadBE32(vb + 24) + offset));
  d.SetW(constant + part * 8 + 4, Insert(LoadBE32(vb + 28) - offset, 1, 28, 2));
}

// void D3DDevice_EndIndexedVertices(D3DDevice*) [0x826561F8]: the inline end of
// BeginIndexedVertices, back to the ring pointer it saved.
void EndIndexedVertices(Device d) {
  if (d)
    d.SetW(kRing, d.W(kIndexedVerticesRing));
}

// void D3DDevice_GetDisplayMode(D3DDevice*, UINT iSwapChain, D3DDISPLAYMODE*) [0x82658960]
void GetDisplayMode(Device d, u32 mode_va) {
  if (!d || !mode_va)
    return;
  u8* mode = Guest(d, mode_va);
  StoreBE32(mode, d.W(kDisplayWidth));
  StoreBE32(mode + 4, d.W(kDisplayHeight));
  StoreBE32(mode + 8, d.W(kDisplayRate));
  StoreBE32(mode + 12, d.W(kDisplayFormat));
}

}  // namespace renut::d3d::port
