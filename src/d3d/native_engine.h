#pragma once
// Native renderer v1: the engine's world draws, taken where the engine issues
// them (native_engine.cpp) and recognised by the Plume renderer when their PM4
// draw reaches it.
#include <cstdint>
#include <string>

namespace renut::native_engine {

// Each tagged draw carries its id in the vertex shader's boolean registers
// 104..127 - bits 8..31 of SHADER_CONSTANT_BOOL_096_127, which no N&B vertex
// shader tests (they test 0..3).
constexpr uint32_t kTagShift = 8;

// What the engine said about one draw (sub_823A9C38's draw record).
struct Draw {
  uint32_t context = 0, model = 0, record = 0, pass = 0;
  uint32_t vertex_buffer = 0, stride = 0, index_buffer = 0, index_count = 0;
  uint32_t vertex_shader = 0, pixel_shader = 0;
  uint32_t vb_words[8] = {}, ib_words[8] = {};  // the D3D objects' first words, for checking the decode
  // The bounding sphere (world centre, radius) of the object being drawn, when
  // the draw comes from the object draw sub_823B0698 (the shadow-map jobs and
  // the scene view job): +16..+24 and +32 of its object.
  bool has_sphere = false;
  float sphere[4] = {};
};

// The object sub_823B0698 is drawing on this thread (game_fixes.cpp), whose
// bounding sphere the draws it makes carry; 0 when it returns.
void SetObject(uint32_t object, const uint8_t* base);

// Shadow-map work the Plume renderer saved, for the report.
void NoteShadowCull();
void NoteShadowBatch(uint32_t instances);
// A draw whose pixel shader is native_lit.frag.
void NoteNativeLit();

// The draw a tag names, if its slot of the ring still holds it.
bool Lookup(uint32_t tag, Draw& out);

// What the Plume renderer found for one tagged PM4 draw: the tag resolved,
// and which words of the engine's objects held the vertex and index buffer
// addresses the PM4 draw used (-1 none).
void NoteDraw(bool found, bool count_matches, int vb_word, int ib_word, bool native_view);
void NoteUntagged();
// One engine world colour draw with these shaders (validated), so that the
// report can rank the materials the native renderer ports first.
void NoteShaders(uint64_t vertex_shader, uint64_t pixel_shader);

// One line of detail for the report (the first two dozen are kept).
void AddSample(const std::string& line);

// Appended to the reconnaissance report (native_probe.cpp); resets the counters.
void Report(std::string& out, uint64_t frames);

}  // namespace renut::native_engine
