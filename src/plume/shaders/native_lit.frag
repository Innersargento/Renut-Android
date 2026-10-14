#version 450
#extension GL_EXT_nonuniform_qualifier : require
// Native renderer: the lit world material of N&B, written by hand from the
// recompiled permutation C139857BC4CA265A (the material that draws the most in
// Showdown Town) and standing in for every guest pixel shader that
// tools/native/lit_table.py proves to be the same permutation
// (native_lit_table.inc). It reads exactly what the guest shader reads - the
// same interpolants, the draw's pixel constant blocks, its textures and
// booleans from the shared block - so it drops into the guest's pipeline as is.
//
// The material, as the engine sets it up:
//   surface    albedo texture (slot 0), specular mask (slot 1), tangent-space
//              normal map (slot 2) on the interpolated TBN (TEXCOORD2..4)
//   sun        Lambert and Blinn-Phong (power: material register kRegPower)
//              from c51, coloured c32, shadowed by three cascades (slots 15,
//              14, 13; rows c65-67, c61-63, c57-59; splits c47; bias c46) of
//              8 taps on a disc turned by a noise texture (slot 12)
//   lights     up to six point/spot lights, c83 + 3k, booleans 243 + 2k
//   ambient    hemisphere c81/c82 and a linear SH c35/c37/c39
//   output     albedo x diffuse + specular x mask x kRegSpecular; boolean 129
//              tints by c45; boolean 130 adds the game's Reinhard curve
layout(constant_id = 0) const uint kSpecConstants = 0u;  // bit 1: alpha test
// The material's registers in this member of the permutation.
layout(constant_id = 110) const uint kRegPower = 0u;
layout(constant_id = 111) const uint kRegSpecular = 5u;
layout(constant_id = 112) const uint kRegAlpha = 6u;
// Whether the cascades subtract their depth bias (c46) before comparing.
layout(constant_id = 113) const uint kShadowBias = 1u;

layout(set = 0, binding = 0) uniform texture2D gTextures[];
layout(set = 3, binding = 0) uniform sampler gSamplers[];
layout(set = 4, binding = 4, std140) uniform PixelConstants0 { vec4 v[64]; } gConst0;
layout(set = 4, binding = 5, std140) uniform PixelConstants1 { vec4 v[64]; } gConst1;
layout(set = 4, binding = 8, std140) uniform Shared { uvec4 w[48]; } gShared;

layout(location = 0) in vec4 iTexCoord;  // xy: the surface's texture coordinates
layout(location = 1) in vec4 iWorld;     // world position
layout(location = 2) in vec4 iNormal;    // the TBN's rows
layout(location = 3) in vec4 iTangent;
layout(location = 4) in vec4 iBinormal;
layout(location = 23) in vec4 iPointSprite;

layout(location = 0) out vec4 oColor;

vec4 C(uint n) { return n < 64u ? gConst0.v[n] : gConst1.v[n - 64u]; }
uint SharedWord(uint offset) { return gShared.w[offset >> 4u][(offset >> 2u) & 3u]; }
bool Bool(uint n) { return ((SharedWord(512u + (n >> 5u) * 4u) >> (n & 31u)) & 1u) != 0u; }
vec4 Fetch(uint slot, vec2 uv) {
    return texture(sampler2D(gTextures[SharedWord(slot * 4u)], gSamplers[SharedWord(384u + slot * 4u)]), uv);
}
float Rsqrt(float x) { return clamp(inversesqrt(x), -3.402823466e38, 3.402823466e38); }
float Rcp(float x) { return clamp(1.0 / x, -3.402823466e38, 3.402823466e38); }
float Log2(float x) { return clamp(log2(x), -3.402823466e38, 3.402823466e38); }

// The disc: four pairs of taps, (x, y) and (z, w) of each.
const vec4 kDisc[4] = vec4[4](
    vec4(0.0, 0.0, 0.574618995, 0.685878992),
    vec4(-0.0400880016, 0.536086977, -0.757088006,
         0.349334002),
    vec4(-0.670445025, -0.179949, -0.419418007,
         -0.616038978),
    vec4(0.440452993, -0.639398992, 0.527836978,
         -0.085868001));

// One cascade: the fraction of the 8 taps whose stored depth is at or beyond
// the fragment's.
float Cascade(uint slot, uint rowU, uint rowV, uint rowDepth, float bias, vec3 rotation) {
    vec2 centre = vec2(dot(iWorld, C(rowU)), dot(iWorld, C(rowV))) * vec2(0.5, -0.5) + vec2(0.5);
    float depth = dot(iWorld, C(rowDepth));
    if (kShadowBias != 0u) depth = depth - bias;
    float lit = 0.0;
    for (int k = 0; k < 4; ++k) {
        vec4 d = kDisc[k];
        vec2 a = centre + d.x * rotation.xy + d.y * rotation.zx;
        vec2 b = centre + d.z * rotation.xy + d.w * rotation.zx;
        lit += (Fetch(slot, a).x >= depth ? 0.125 : 0.0) + (Fetch(slot, b).x >= depth ? 0.125 : 0.0);
    }
    return lit;
}

void main() {
    vec2 uv = iTexCoord.xy;

    // The surface normal from the normal map.
    vec2 tangent = Fetch(2u, uv).xy * 2.0 - 1.0;
    vec3 ts = vec3(tangent, sqrt(abs(dot(tangent, -tangent) + 1.0)));
    vec3 n = vec3(dot(iTangent.xyz, ts), dot(iBinormal.xyz, ts), dot(iNormal.xyz, ts));
    n = n * Rsqrt(dot(n, n));

    // Sun shadow. The screen position as the guest reads it (renutFragPos, the
    // y axis flipped for anything that is not a point sprite).
    float split = uintBitsToFloat(SharedWord(584u));
    vec2 frag = vec2(gl_FragCoord.x, gl_FragCoord.y - ((split > 0.0 && gl_FragCoord.y >= split) ? split : 0.0));
    vec2 screen = (frag - 0.5) *
                  vec2(iPointSprite.x < 0.0 ? 1.0 : (gl_FrontFacing ? 1.0 : -1.0), iPointSprite.x < 0.0 ? -1.0 : 1.0);
    vec2 noise = Fetch(12u, screen * 0.03125).xy;
    vec3 rotation = vec3(noise.y, noise.x, noise.x) *
                        vec3(0.00195312989, 0.00195312989, -0.00195312989) +
                    vec3(-0.000976559997, -0.000976559997, 0.000976559997);
    vec3 splits = -vec3(dot(iWorld, C(55u))) - C(47u).xyz;
    float shadow;
    if (splits.z > 0.0) {
        shadow = Cascade(15u, 65u, 66u, 67u, C(46u).w, rotation * 0.25);
    } else if (splits.y > 0.0) {
        shadow = Cascade(14u, 61u, 62u, 63u, C(46u).z, rotation);
    } else {
        shadow = Cascade(13u, 57u, 58u, 59u, C(46u).y, rotation);
    }

    // Sun.
    vec3 toEye = C(52u).xyz - iWorld.xyz;
    toEye = toEye * Rsqrt(dot(toEye, toEye));
    float power = C(kRegPower).x;
    vec3 diffuse = vec3(clamp(dot(C(51u).xyz, n), 0.0, 1.0));
    vec3 half_ = toEye + C(51u).xyz;
    half_ = half_ * Rsqrt(dot(half_, half_));
    vec3 specular = vec3(exp2(Log2(clamp(dot(half_, n), 0.0, 1.0)) * power));
    vec3 sun = shadow * C(32u).xyz;
    diffuse *= sun;
    specular *= sun;

    // Point and spot lights, each enabled only with the one before it.
    if (Bool(243u)) {
        vec3 reflected = n * (2.0 * dot(toEye, n)) - toEye;
        for (uint k = 0u; k < 6u; ++k) {
            if (k > 0u && !Bool(243u + 2u * k)) break;
            uint base = 83u + 3u * k;
            vec3 toLight = C(base).xyz - iWorld.xyz;
            float distance = sqrt(dot(toLight, toLight));
            float attenuation = clamp(-distance * C(base).w + C(base + 1u).w, 0.0, 1.0);
            bool on = attenuation > 0.0;
            float nDotL = 0.0, logSpecular = 0.0;
            if (on) {
                toLight *= Rcp(distance);
                logSpecular = Log2(clamp(dot(toLight, reflected), 0.0, 1.0));
                nDotL = dot(toLight, n);
            }
            if (Bool(244u + 2u * k)) {
                float spot = 0.0;
                if (on) spot = clamp(3.0 * clamp(dot(toLight, -C(base + 2u).xyz) - C(base + 2u).w, 0.0, 1.0), 0.0, 1.0);
                on = on && spot > 0.0;
                attenuation *= spot;
            }
            if (on) {
                diffuse += clamp(nDotL * attenuation, 0.0, 1.0) * C(base + 1u).xyz;
                specular += attenuation * exp2(power * logSpecular) * C(base + 1u).xyz;
            }
        }
    }

    // Surface.
    specular *= Fetch(1u, uv).xyz * C(kRegSpecular).xyz;
    diffuse += clamp(dot(C(82u).xyz, n), 0.0, 1.0) * C(81u).xyz;
    vec4 albedo = Fetch(0u, uv);
    vec4 sh = vec4(1.0, n.y, n.z, n.x);
    diffuse += vec3(dot(sh, C(35u)), dot(sh, C(37u)), dot(sh, C(39u)));
    float diffuseAlpha = 1.0;
    if (!Bool(129u)) diffuseAlpha = C(kRegAlpha).w;
    if (Bool(129u)) {
        albedo.xyz *= C(45u).xyz;
        albedo.w = C(45u).w;
    }
    vec4 colour = albedo * vec4(diffuse, diffuseAlpha) + vec4(specular, 0.0);
    if (Bool(130u)) {
        vec3 denominator = colour.xyz * C(45u).xyz + 1.0;
        colour.xyz *= 1.5;
        colour *= C(45u);
        colour.xyz *= vec3(Rcp(denominator.x), Rcp(denominator.y), Rcp(denominator.z));
    }
    if ((kSpecConstants & 2u) != 0u && colour.w - uintBitsToFloat(SharedWord(556u)) < 0.0) discard;
    oColor = colour;
}
