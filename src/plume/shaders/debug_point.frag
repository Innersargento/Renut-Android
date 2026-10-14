#version 450
// Debug view for plume-debug-shader mode 8: the point-sprite coordinate as
// red and green, and blue where the first interpolator (a sprite's rotation
// in N&B's particle shaders) is non-zero. Compiled by
// tools/shaders/build_host_shaders.sh.
layout(location = 0) in vec4 iTexCoord0;
layout(location = 23) in vec4 iPointSprite;
layout(location = 0) out vec4 oColor;

void main()
{
    oColor = vec4(clamp(iPointSprite.z, 0.0, 1.0), clamp(iPointSprite.w, 0.0, 1.0),
                  dot(iTexCoord0.xyz, iTexCoord0.xyz) > 0.01 ? 1.0 : 0.0, 1.0);
}
