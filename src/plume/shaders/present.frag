#version 450
// The frame shown on the swapchain image (present.vert).

layout(push_constant) uniform Present {
  vec4 turn;
  vec2 extent;
  vec2 uv_scale;
  vec2 uv_limit;
} present;

layout(set = 0, binding = 0) uniform sampler2D frame;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() {
  color = vec4(textureLod(frame, min(uv, present.uv_limit), 0.0).rgb, 1.0);
}
