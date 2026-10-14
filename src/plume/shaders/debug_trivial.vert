#version 450
// plume-trivial mode 1: stands in for a draw's vertex shader, to price what a
// draw costs the GPU apart from the guest's shaders. Every primitive collapses
// to a point and is culled, so the pixel shader never runs either.
void main() {
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
