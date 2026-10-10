#version 450
#extension GL_EXT_nonuniform_qualifier : require
// Native renderer v1, native surfaces (plume-native 3): the pixel shader of an
// engine world draw of any vertex shader family. The guest vertex shader stays
// - it places every family exactly (instanced, skinned, live fetch) - and this
// replaces the guest material: the surface texture (native_ps_table.inc: the
// slot, and the interpolant INTERPOLANT its coordinates come in, as the guest
// pixel shader reads them) through the bindless heap, with the guest's alpha
// test. Compiled once per interpolant 0..3.
//
// The scene's alpha is the game's bloom mask, so an opaque draw writes 0.
layout(set = 0, binding = 0) uniform texture2D gTextures[];
layout(set = 3, binding = 0) uniform sampler gSamplers[];

layout(push_constant) uniform Native {
    layout(offset = 112) float alphaRef;  // 0 = no alpha test
    layout(offset = 116) uint flags;      // bit 0: a texture is bound; bit 1: the draw blends
    layout(offset = 120) uint texture;
    layout(offset = 124) uint samplerIndex;
} pc;

layout(location = INTERPOLANT) in vec4 iTexCoord;

layout(location = 0) out vec4 oColor;

void main() {
    vec4 c = (pc.flags & 1u) != 0u
                 ? texture(sampler2D(gTextures[pc.texture], gSamplers[pc.samplerIndex]), iTexCoord.xy)
                 : vec4(0.6, 0.6, 0.6, 1.0);
    if (pc.alphaRef > 0.0 && c.a < pc.alphaRef) discard;
    oColor = vec4(c.rgb * 0.85, (pc.flags & 2u) != 0u ? c.a : 0.0);
}
