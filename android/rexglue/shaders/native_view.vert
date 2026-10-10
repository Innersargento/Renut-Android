#version 450
// Native renderer v1, native view (plume-native 2): a world draw of the engine
// (tagged by native_engine.cpp) whose guest vertex shader is a rigid transform
// is drawn entirely with the renderer's own shaders - this one and
// native_view.frag: the position by the engine's world-view-projection, the
// first texture coordinate, and a light from the normal. Compiled four times,
// with and without HAS_TEXCOORD (location 8) and HAS_NORMAL (location 5), so
// that it reads exactly the inputs the draw's vertex layout has.
//
// The input decode is the recompiled shaders' (unpackInput, swapFloats,
// sintTexcoord, tfetchR11G11B10 in shader_common.h), driven by the same
// specialization constants.
layout(constant_id = 0) const uint kSpecConstants = 0u;  // bit 0: R11G11B10 normals
layout(constant_id = 1) const uint kPackedInputs = 0u;
layout(constant_id = 2) const uint kIntInputs = 0u;
layout(constant_id = 4) const uint kSwappedTexcoords = 0u;
layout(constant_id = 5) const uint kSwappedNormals = 0u;
layout(constant_id = 9) const uint kSwappedPositions = 0u;
layout(constant_id = 11) const uint kSintTexcoords = 0u;
layout(constant_id = 16) const uint kRemap0 = 0u;
layout(constant_id = 21) const uint kRemap5 = 0u;
layout(constant_id = 24) const uint kRemap8 = 0u;
// The guest shader's swizzles, 2 bits a component.
layout(constant_id = 100) const uint kPositionSwizzle = 36u;
layout(constant_id = 101) const uint kTexcoordSwizzle = 4u;
layout(constant_id = 102) const uint kNormalSwizzle = 36u;

layout(location = 0) in vec4 iPosition;
#ifdef HAS_NORMAL
layout(location = 5) in vec4 iNormal;
#endif
#ifdef HAS_TEXCOORD
layout(location = 8) in vec4 iTexCoord;
#endif

layout(push_constant) uniform Native {
    layout(offset = 32) vec4 rows[4];   // world-view-projection (c0..c3)
    layout(offset = 96) vec4 viewport;  // xy scale, zw offset (g_ViewportScaleOffset)
} pc;

layout(location = 0) out vec2 oTexCoord;
layout(location = 1) out float oLight;

out gl_PerVertex {
    vec4 gl_Position;
};

vec4 remap(vec4 value, uint code) {
    if (code == 0u) return value;
    code ^= 0x688u;
    vec4 result;
    for (uint i = 0u; i < 4u; ++i) {
        uint source = (code >> (3u * i)) & 7u;
        result[i] = source < 4u ? value[source] : (source == 5u ? 1.0 : 0.0);
    }
    return result;
}

vec4 unpackInput(vec4 value, uint location, uint code) {
    if ((kPackedInputs & (1u << location)) != 0u) {
        uint v = floatBitsToUint(value.x);
        ivec4 s = ivec4(int(v << 22u) >> 22, int(v << 12u) >> 22, int(v << 2u) >> 22, int(v) >> 30);
        value = max(vec4(s) * vec4(0.001956947147846221923828125, 0.001956947147846221923828125,
                                   0.001956947147846221923828125, 1.0), vec4(-1.0));
    } else if ((kIntInputs & (1u << location)) != 0u) {
        value = vec4(floatBitsToInt(value));
    }
    return remap(value, code);
}

float pick(vec4 v, uint swizzle, uint component) {
    return v[(swizzle >> (2u * component)) & 3u];
}

void main() {
    vec4 value = unpackInput(iPosition, 0u, kRemap0);
    if ((kSwappedPositions & 1u) != 0u) value = value.yxwz;
    vec4 p = vec4(pick(value, kPositionSwizzle, 0u), pick(value, kPositionSwizzle, 1u),
                  pick(value, kPositionSwizzle, 2u), 1.0);
    vec4 o = vec4(dot(p, pc.rows[0]), dot(p, pc.rows[1]), dot(p, pc.rows[2]), dot(p, pc.rows[3]));

#ifdef HAS_TEXCOORD
    vec4 t = unpackInput(iTexCoord, 8u, kRemap8);
    if ((kSwappedTexcoords & 1u) != 0u) t = t.yxwz;
    if ((kSintTexcoords & 1u) != 0u) t = vec4((ivec4(floatBitsToUint(t)) << 16) >> 16);
    oTexCoord = vec2(pick(t, kTexcoordSwizzle, 0u), pick(t, kTexcoordSwizzle, 1u));
#else
    oTexCoord = vec2(0.0);
#endif

#ifdef HAS_NORMAL
    vec4 n = unpackInput(iNormal, 5u, kRemap5);
    if ((kSwappedNormals & 1u) != 0u) n = n.yxwz;
    if ((kSpecConstants & 1u) != 0u) {
        uint v = floatBitsToUint(n.x);
        n = vec4(((v & 0x400u) != 0u ? -1.0 : 0.0) + float(v & 0x3FFu) / 1024.0,
                 ((v & 0x200000u) != 0u ? -1.0 : 0.0) + float((v >> 11u) & 0x3FFu) / 1024.0,
                 ((v & 0x80000000u) != 0u ? -1.0 : 0.0) + float((v >> 22u) & 0x1FFu) / 512.0, 0.0);
    }
    vec3 normal = vec3(pick(n, kNormalSwizzle, 0u), pick(n, kNormalSwizzle, 1u), pick(n, kNormalSwizzle, 2u));
    // Towards the camera through the projection's rows: a head-light, enough to
    // read the shapes.
    vec3 towards = vec3(dot(normal, pc.rows[0].xyz), dot(normal, pc.rows[1].xyz), dot(normal, pc.rows[3].xyz));
    oLight = 0.45 + 0.55 * abs(normalize(towards + vec3(0.0, 0.0, 1e-6)).z);
#else
    oLight = 1.0;
#endif

    o.xy = o.xy * pc.viewport.xy + pc.viewport.zw * o.w;
    o.y = -o.y;
    gl_Position = o;
}
