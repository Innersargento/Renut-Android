#version 450
// plume-trivial mode 2: stands in for a draw's pixel shader (see
// debug_trivial.vert). Writes a flat colour to every target.
layout(location = 0) out vec4 color0;
layout(location = 1) out vec4 color1;
layout(location = 2) out vec4 color2;
layout(location = 3) out vec4 color3;
void main() {
    color0 = color1 = color2 = color3 = vec4(1.0, 0.0, 1.0, 1.0);
}
