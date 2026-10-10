#pragma once
// Native Direct3D front end: the game's Direct3D draws, state flushes, clears
// and resolves turned into host command lists instead of PM4.
//
// The Xbox 360 Direct3D keeps the whole GPU state in the device (a shadow of
// the registers, with dirty bits) and only writes it out, as PM4, when a draw
// needs it (the SetPending_* functions). The game sets state by writing the
// device directly - much of it inlined into game code by LTCG - so the device
// is the one place the full state lives. Here the draw functions are replaced:
// their state flush is done again from the device, following the PowerPC
// (sub_826567C8, sub_8222BF10, ...), into records of a host list, and the
// draw itself becomes a record. Command buffers get a list of their own,
// which a run appends to the list being recorded. Control flow (fences, ring
// segments, swaps, the command buffers' own memory) stays with the game's
// Direct3D: the PM4 it still writes carries no rendering.
//
// renut_native_d3d: 0 off, 1 verify (the native lists are built next to the
// game's PM4, which still renders; the Plume command processor checks every
// PM4 draw against the native one and writes files/native-d3d-verify.txt),
// 2 native (the lists render, PM4 draws are not issued).
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <rex/ppc/context.h>

namespace renut::native_d3d {

enum class Mode : uint32_t { kOff = 0, kVerify = 1, kNative = 2 };
Mode GetMode();

// Record opcodes, in the low 8 bits of a record's first word; the upper 24
// bits hold a count or flags.
enum Op : uint32_t {
  kOpRegs = 1,     // [op | count << 8] first register, count values
  kOpShader = 2,   // [op] type | physical address, dwords, hash lo, hash hi (verify only)
  kOpDraw = 3,     // [op | flags << 8] viz token, initiator, dma base, dma size, serial
  kOpBin = 4,      // [op | which << 8] value: 0 mask lo, 1 mask hi, 2 select lo, 3 select hi
  kOpRun = 5,      // [op] index into List::runs
  kOpMarker = 6,   // [op | kind << 8] serial: a Direct3D call whose draws the list does not hold
  kOpSwap = 7,     // [op] frontbuffer, width, height
  // [op | flags << 8] x1, y1, x2, y2, red, green, blue, alpha, z (float bits), stencil,
  // RB_SURFACE_INFO, RB_COLOR_INFO of targets 0-3, RB_DEPTH_INFO, present (bits 0-3 targets, 4 depth)
  kOpClear = 9,
  // [op] x0, y0, x1, y1 (resolve-aligned), RB_COPY_CONTROL .. RB_COPY_SURFACE_SLICE
  // (0x2318-0x2323), RB_SURFACE_INFO .. RB_COLOR3_INFO (0x2000-0x2005), source slot
  // (0-3 render target, 4 depth)
  kOpResolve = 10,
  // [op | slots << 8] for each slot set (bits 0-3 render targets 0-3, 4 depth),
  // in order: the D3DSurface bound there, its +24 (RB_SURFACE_INFO), +28
  // (RB_COLOR_INFO / RB_DEPTH_INFO) and +36 (width - 1 << 18 | height - 1 << 3).
  // Comes with every send of the target registers it stands behind.
  kOpSurfaces = 11,
};
constexpr uint32_t kResolveWords = 24, kClearWords = 18;

enum DrawFlags : uint32_t {
  kDrawPredicated = 1,    // obeys the bin mask, as every Direct3D draw packet does
  kDrawScreenExtent = 2,  // took Direct3D's screen extent path (not reproduced)
};

enum MarkerKind : uint32_t {
  kMarkClear = 1,
  kMarkResolve = 2,
  kMarkTessellated = 3,
  kMarkExport = 4,
  kMarkIndexedUP = 5,
  kMarkGprAllocation = 6,  // SetShaderGPRAllocation: 24 one-point draws that drain the GPU
};

struct List {
  std::vector<uint32_t> words;
  std::vector<std::shared_ptr<const List>> runs;
  uint32_t draws = 0;
  uint32_t command_buffer = 0;  // guest object, 0 for the immediate list
};

// Called by the Direct3D interception layer (d3d_layer.cpp) for every
// intercepted function, before and after the original. OnEnter returns true
// when it handled the call, and the original is skipped.
bool OnEnter(uint32_t function, PPCContext& ctx, uint8_t* base);
void OnLeave(uint32_t function, uint32_t r3, PPCContext& ctx, uint8_t* base);

// The command processor's side: the immediate lists published so far, in
// order (each holds what the game recorded up to a ring kick-off).
void TakePublished(std::vector<std::shared_ptr<const List>>& out);

// Verify mode, called by the Plume command processor.
struct Pm4Draw {
  const uint32_t* regs = nullptr;  // the register file
  uint64_t vs_hash = 0, ps_hash = 0;
  uint32_t vs_dwords = 0, ps_dwords = 0;
  uint32_t initiator = 0, dma_base = 0, dma_size = 0;
  bool indexed = false;
  uint64_t bin_mask = 0, bin_select = 0;
  uint32_t edram_mode = 0;
  uint64_t frame = 0;
};
void VerifyDraw(const Pm4Draw& draw);
// A predicated PM4 draw the bin check skipped.
void VerifySkipped(uint64_t bin_mask, uint64_t bin_select);
// A NOP marker the front end wrote around an opaque call (payload word).
void VerifyMarker(uint32_t payload);
// The serial in the NOP ahead of a PM4 draw (payload 0xFF000000 | serial).
void VerifyTag(uint32_t serial);
// A PM4 swap: the native lists must be at their swap too.
void VerifySwap(uint64_t frame);
// The magic word of the front end's NOP markers (verify mode).
constexpr uint32_t kNopMagic = 0x4E415444;  // "NATD"
// Native mode: the executor's counters, written with the front end's to
// files/native-d3d-verify-native.txt every 300 frames.
void NativeReport(uint64_t frame, uint64_t draws, uint64_t ring_draws_dropped, uint64_t markers, uint64_t bad,
                  const std::function<std::string()>& executor);
// Where the report goes (the runtime's files directory).
void SetFilesDirectory(std::string directory);

}  // namespace renut::native_d3d
