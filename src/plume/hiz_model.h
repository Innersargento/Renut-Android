#pragma once
// Hi-Z support, part 1: what a vertex shader does to a vertex position.
//
// For a vertex shader whose clip-space position is an affine function of one
// fetched vertex element (rigid meshes: the model-view-projection product, with
// or without a dequantisation scale in front), the microcode is executed
// symbolically once. The result is, for each of the four clip components, five
// small expressions over the shader's float constants (the bias and the factor
// of each of the element's x, y, z, w). A draw then evaluates them with its own
// constants - a few hundred multiply-adds, no vertex touched - and has the
// object's clip-space footprint from the mesh's object-space box.
//
// Anything the symbolic executor does not follow exactly (skinning, constant
// addressing by a0 or aL, loops, predication, comparisons feeding the position,
// two fetched elements multiplied together) makes the model invalid, and the
// draw is simply never culled.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace renut::hiz {

// The fetched element the clip position is a function of.
struct FetchDesc {
  uint32_t fetch_constant = 0;  // vertex fetch constant [0, 95]; the stream is 95 - this
  uint32_t format = 0;          // xenos::VertexFormat
  bool is_signed = false;
  bool normalized = false;
  bool signed_no_zero = false;  // SignedRepeatingFractionMode::kNoZero
  bool index_rounded = false;
  int32_t exp_adjust = 0;
  uint32_t stride_dwords = 0;
  int32_t offset_dwords = 0;
  // Components of the fetched vec4 (bit i = x, y, z, w) the position depends on.
  uint32_t used_mask = 0;
};

struct Model {
  struct Node {
    uint8_t op = 0;
    uint16_t a = 0, b = 0;
    float imm = 0.f;
  };
  bool valid = false;
  FetchDesc fetch;
  std::vector<Node> nodes;
  // coeff[o][j]: node of clip component o (x, y, z, w) for j = 0 the bias, 1..4
  // the factor of the fetched x, y, z, w.
  uint16_t coeff[4][5] = {};
  // The number of float constants the program reads (for statistics).
  uint32_t constants_read = 0;
};

// Symbolically executes `ucode` (vertex shader microcode in host order, dword
// count `dwords`) under the boolean constants `bools` (8 words of 32). Returns
// an invalid model when the position is not an affine function of one element.
std::shared_ptr<Model> BuildModel(const uint32_t* ucode, size_t dwords, const uint32_t* bools);

// Evaluates the model's coefficients with the float constants of `constants`
// (the register file's vertex constant block, 4 floats per register, indexed by
// register + `base`). out[o][j] as Model::coeff.
void Evaluate(const Model& model, const float* constants, uint32_t base, float out[4][5]);

// One vertex's fetched element as the model reads it: out[0..3] = x, y, z, w
// (only the components in model.fetch.used_mask are set). `memory` is the base
// of guest physical memory; the stream is `fetch_address` (dwords) of
// `fetch_size` dwords with the given byte order (xenos::Endian).
bool FetchVertex(const FetchDesc& fetch, const uint8_t* memory, uint32_t fetch_address_dwords,
                 uint32_t fetch_size_dwords, uint32_t endian, uint32_t vertex_index, float out[4]);

}  // namespace renut::hiz
