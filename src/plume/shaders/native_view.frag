#version 450
#extension GL_EXT_nonuniform_qualifier : require
// Native renderer v1, native view (plume-native 2): the pixel side of
// native_view.vert. The surface texture (the slot native_ps_table.inc names,
// through the same bindless heap and sampler the guest draw got) times the
// vertex shader's light, with the guest's alpha test. Its indices come in the
// push constants, so the draw reads no guest constant block.
//
// The scene's alpha is the game's bloom mask - its materials write 0 there
// except on what glows - so an opaque draw writes 0; only a blended one keeps
// the texture's alpha for its blend.
layout(set = 0, binding = 0) uniform texture2D gTextures[];
layout(set = 3, binding = 0) uniform sampler gSamplers[];

layout(push_constant) uniform Native {
    layout(offset = 112) float alphaRef;  // 0 = no alpha test
    layout(offset = 116) uint flags;      // bit 0: a texture is bound; bit 1: the draw blends
    layout(offset = 120) uint texture;
    layout(offset = 124) uint samplerIndex;
} pc;

layout(location = 0) in vec2 iTexCoord;
layout(location = 1) in float iLight;

layout(location = 0) out vec4 oColor;

void main() {
    vec4 c = (pc.flags & 1u) != 0u
                 ? texture(sampler2D(gTextures[pc.texture], gSamplers[pc.samplerIndex]), iTexCoord)
                 : vec4(0.7, 0.7, 0.7, 1.0);
    if (pc.alphaRef > 0.0 && c.a < pc.alphaRef) discard;
    oColor = vec4(c.rgb * iLight, (pc.flags & 2u) != 0u ? c.a : 0.0);
}
