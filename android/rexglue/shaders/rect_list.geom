#version 450
// Xenos rectangle lists: every three vertices describe a rectangle, and the
// hardware makes up the fourth. Two of the three share the right-angle corner's
// neighbours; the corner is the vertex opposite the longest edge, and the
// missing vertex is the sum of the corner's two neighbours minus the corner -
// for the position and for every interpolator alike.
//
// Interpolators are locations 0..22, the RENUT_INTERPOLATORS list of the
// recompiled shaders (TEXCOORD0..15, COLOR0..1, NORMAL0..2, TANGENT0,
// BINORMAL0). Compiled by tools/shaders/build_host_shaders.sh.

layout(triangles) in;
layout(triangle_strip, max_vertices = 4) out;

#define INTERPOLATOR(n) \
    layout(location = n) in vec4 i##n[]; \
    layout(location = n) out vec4 o##n;

INTERPOLATOR(0) INTERPOLATOR(1) INTERPOLATOR(2) INTERPOLATOR(3)
INTERPOLATOR(4) INTERPOLATOR(5) INTERPOLATOR(6) INTERPOLATOR(7)
INTERPOLATOR(8) INTERPOLATOR(9) INTERPOLATOR(10) INTERPOLATOR(11)
INTERPOLATOR(12) INTERPOLATOR(13) INTERPOLATOR(14) INTERPOLATOR(15)
INTERPOLATOR(16) INTERPOLATOR(17) INTERPOLATOR(18) INTERPOLATOR(19)
INTERPOLATOR(20) INTERPOLATOR(21) INTERPOLATOR(22)

#define COPY(n, v) o##n = i##n[v];
#define COPY_ALL(v) \
    COPY(0, v) COPY(1, v) COPY(2, v) COPY(3, v) COPY(4, v) COPY(5, v) COPY(6, v) COPY(7, v) \
    COPY(8, v) COPY(9, v) COPY(10, v) COPY(11, v) COPY(12, v) COPY(13, v) COPY(14, v) \
    COPY(15, v) COPY(16, v) COPY(17, v) COPY(18, v) COPY(19, v) COPY(20, v) COPY(21, v) COPY(22, v)

#define FOURTH(n, a, b, c) o##n = i##n[a] + i##n[b] - i##n[c];
#define FOURTH_ALL(a, b, c) \
    FOURTH(0, a, b, c) FOURTH(1, a, b, c) FOURTH(2, a, b, c) FOURTH(3, a, b, c) \
    FOURTH(4, a, b, c) FOURTH(5, a, b, c) FOURTH(6, a, b, c) FOURTH(7, a, b, c) \
    FOURTH(8, a, b, c) FOURTH(9, a, b, c) FOURTH(10, a, b, c) FOURTH(11, a, b, c) \
    FOURTH(12, a, b, c) FOURTH(13, a, b, c) FOURTH(14, a, b, c) FOURTH(15, a, b, c) \
    FOURTH(16, a, b, c) FOURTH(17, a, b, c) FOURTH(18, a, b, c) FOURTH(19, a, b, c) \
    FOURTH(20, a, b, c) FOURTH(21, a, b, c) FOURTH(22, a, b, c)

void emitVertex(int v)
{
    gl_Position = gl_in[v].gl_Position;
    COPY_ALL(v)
    EmitVertex();
}

void emitRectangle(int corner, int a, int b)
{
    // Strip corner, a, b, a + b - corner covers the rectangle.
    emitVertex(corner);
    emitVertex(a);
    emitVertex(b);
    gl_Position = gl_in[a].gl_Position + gl_in[b].gl_Position - gl_in[corner].gl_Position;
    FOURTH_ALL(a, b, corner)
    EmitVertex();
    EndPrimitive();
}

void main()
{
    vec2 p0 = gl_in[0].gl_Position.xy / gl_in[0].gl_Position.w;
    vec2 p1 = gl_in[1].gl_Position.xy / gl_in[1].gl_Position.w;
    vec2 p2 = gl_in[2].gl_Position.xy / gl_in[2].gl_Position.w;
    float d01 = dot(p1 - p0, p1 - p0);
    float d02 = dot(p2 - p0, p2 - p0);
    float d12 = dot(p2 - p1, p2 - p1);
    if (d12 >= d01 && d12 >= d02)
        emitRectangle(0, 1, 2);
    else if (d02 >= d01)
        emitRectangle(1, 0, 2);
    else
        emitRectangle(2, 0, 1);
}
