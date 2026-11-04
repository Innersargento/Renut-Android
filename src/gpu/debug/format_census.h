#pragma once
// Format census: which guest formats the game really uses, to decide what the
// guest_format module (see docs/xenos-removal-plan.md, phase 3) can drop.
//
// Every call is a counter bump behind a mutex or an atomic; the cold ones (one
// per distinct texture constant, per shader, per target) take the mutex, the
// per-draw one (Primitive) is an atomic. Dump() writes <files>/used-formats.txt
// and runs from the swap every kDumpEverySwaps frames, so a run killed by the
// system still leaves the file behind.
#include <cstdint>
#include <string>

#include "gpu/core/base.h"

namespace renut::gpu::format_census {

constexpr uint64_t kDumpEverySwaps = 300;

// A texture constant seen by LayoutOfByContents (one per distinct constant).
void Texture(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch);
// A vertex element of a shader, decoded once per shader (format is a
// VertexFormat value, which shares the TextureFormat numbering).
void Vertex(uint32_t format, bool is_signed, bool normalized);
// The destination format of an EDRAM resolve.
void Resolve(uint32_t dest_format);
// The surface formats of a created render target.
void ColorTarget(uint32_t format);
void DepthTarget(uint32_t format);
// A draw's primitive type and index size (per draw: atomics only).
void Primitive(uint32_t prim_type, uint32_t index_size_bytes);

// Writes the report next to the miss dump directory, if it is set.
void Dump(const std::string& miss_dump_directory, uint64_t swap);

}  // namespace renut::gpu::format_census
