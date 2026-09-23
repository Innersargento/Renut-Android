#version 450
// Xenos point lists: the hardware turns every point into a screen-aligned
// sprite. Its diameter comes from the vertex shader's point size export, or
// PA_SU_POINT_SIZE when it has none, and the recompiled vertex shader already
// turned that into a radius in host NDC at the point-sprite location (see
// RENUT_POINT_SPRITE_LOCATION in shader_common.h). The same interpolators go to
// all four corners; the point-sprite location becomes (-1, 0, s, t), which the
// pixel shader puts in its param-gen register, with (0, 0) at the top left.
//
// Interpolators are locations 0..22, the RENUT_INTERPOLATORS list of the
// recompiled shaders, and 23 is the point sprite. Compiled by
// tools/shaders/build_host_shaders.sh.

layout(points) in;
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

layout(location = 23) in vec4 iPointSprite[];
layout(location = 23) out vec4 oPointSprite;

#define COPY(n) o##n = i##n[0];
#define COPY_ALL() \
    COPY(0) COPY(1) COPY(2) COPY(3) COPY(4) COPY(5) COPY(6) COPY(7) \
    COPY(8) COPY(9) COPY(10) COPY(11) COPY(12) COPY(13) COPY(14) \
    COPY(15) COPY(16) COPY(17) COPY(18) COPY(19) COPY(20) COPY(21) COPY(22)

void main()
{
    vec4 position = gl_in[0].gl_Position;
    // A point is clipped as a point: one whose centre is behind the near
    // plane or past the far one draws nothing. Expanded regardless, a
    // particle brushing past the camera would become a sprite hundreds of
    // pixels wide - its size divides by w - reaching in from the edges.
    if (!(position.w > 0.0 && position.z >= 0.0 && position.z <= position.w))
        return;
    // Signed like the viewport: position.y - radius.y is the top of the screen.
    vec2 radius = iPointSprite[0].xy;
    // A point of size zero draws nothing; point lists double as a way to run
    // a vertex shader for its memory export alone.
    if (!(radius.x > 0.0 && radius.y != 0.0))
        return;
    radius *= position.w;
    // Top left, bottom left, top right, bottom right.
    for (int i = 0; i < 4; ++i)
    {
        vec2 corner = vec2(float(i >> 1), float(i & 1));
        gl_Position = vec4(position.xy + (corner * 2.0 - 1.0) * radius, position.zw);
        COPY_ALL()
        oPointSprite = vec4(-1.0, 0.0, corner);
        EmitVertex();
    }
    EndPrimitive();
}
