#version 450
// The D3D library's own pixel shader (9 dwords, 0x8206B098): oC0 = interpolator 0.
layout(location = 0) in vec4 i0;
layout(location = 0) out vec4 color;
void main() { color = i0; }
