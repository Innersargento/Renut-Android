#version 450
// The frame shown on the swapchain image (PlumeRenderer::SubmitAndPresent): a strip of four vertices over the
// letterboxed rectangle, turned by the swapchain's pre-transform.

layout(push_constant) uniform Present {
  vec4 turn;      // the 2x2 matrix that turns the picture as the swapchain's pre-transform says, by columns
  vec2 extent;    // half the rectangle's size, in the clip space of the picture before it is turned
  vec2 uv_scale;  // the part of the texture the frame takes
  vec2 uv_limit;  // the centre of the frame's last texel: nothing past it is filtered in
} present;

layout(location = 0) out vec2 uv;

void main() {
  const vec2 corner = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
  uv = corner * present.uv_scale;
  const vec2 place = (corner * 2.0 - 1.0) * present.extent;
  gl_Position = vec4(mat2(present.turn.xy, present.turn.zw) * place, 0.0, 1.0);
}
