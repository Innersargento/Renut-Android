#version 450
// Native renderer v0: the vertex shader of a depth-only draw whose guest
// shader is a rigid transform (tools/native/scan_native_vs.py lists them in
// native_vs_table.inc). It computes the same position from the same input,
// but every value it needs per draw arrives in the push constants: it reads
// none of the guest constant blocks, so the draw needs no uniform buffer, no
// descriptor rebinding and no preamble loading constants.
//
// The input decode is the recompiled shaders' (unpackInput, swapFloats in
// shader_common.h) for location 0, driven by the same specialization
// constants the host already passes them.
layout(constant_id = 1) const uint kPackedInputs = 0u;
layout(constant_id = 2) const uint kIntInputs = 0u;
layout(constant_id = 9) const uint kSwappedPositions = 0u;
layout(constant_id = 16) const uint kInputRemap0 = 0u;
// The guest shader's swizzle of the fetched position, 2 bits a component.
layout(constant_id = 100) const uint kSwizzle = 36u;

layout(location = 0) in vec4 iPosition;

layout(push_constant) uniform Native {
    layout(offset = 32) vec4 rows[4];   // the guest's c0..c3 (or whichever four it names)
    layout(offset = 96) vec4 viewport;  // xy scale, zw offset (g_ViewportScaleOffset)
    layout(offset = 112) vec2 halfPixel;
} pc;

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

void main() {
    vec4 value = iPosition;
    if ((kPackedInputs & 1u) != 0u) {
        uint v = floatBitsToUint(value.x);
        ivec4 s = ivec4(int(v << 22u) >> 22, int(v << 12u) >> 22, int(v << 2u) >> 22, int(v) >> 30);
        value = max(vec4(s) * vec4(0.001956947147846221923828125, 0.001956947147846221923828125,
                                   0.001956947147846221923828125, 1.0), vec4(-1.0));
    } else if ((kIntInputs & 1u) != 0u) {
        value = vec4(floatBitsToInt(value));
    }
    value = remap(value, kInputRemap0);
    if ((kSwappedPositions & 1u) != 0u) value = value.yxwz;
    vec4 p = vec4(value[kSwizzle & 3u], value[(kSwizzle >> 2u) & 3u], value[(kSwizzle >> 4u) & 3u], 1.0);
    vec4 o = vec4(dot(p, pc.rows[0]), dot(p, pc.rows[1]), dot(p, pc.rows[2]), dot(p, pc.rows[3]));
    o.xy = o.xy * pc.viewport.xy + (pc.viewport.zw + pc.halfPixel) * o.w;
    o.y = -o.y;
    gl_Position = o;
}
