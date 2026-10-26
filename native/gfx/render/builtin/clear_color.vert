#version 450
// The D3D library's own rectangle shader (27 dwords of microcode, found at 0x8206B0C0 of the game): POSITION0 (3 floats, stride 7 dwords) and COLOR0 (4 floats) passed through;
// interpolator 0 = the colour. Used for the draws that clear a part of a render target (colour and/or depth written by an always-passing depth test). Positions are window coordinates
// when the viewport transform is off: they go through the same NDC mapping the translated vertex shaders get (shader patch 0003: g_NdcScale / g_HalfPixelOffset in the shared constants).
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(buffer_reference, std430) readonly buffer Shared { uint dwords[]; };
layout(push_constant) uniform PC { uint64_t vs; uint64_t ps; Shared sh; } pc;
layout(location = 0) in vec4 i_pos;
layout(location = 8) in vec4 i_color;
layout(location = 0) out vec4 o[18];
void main() {
  const vec2 scale = vec2(uintBitsToFloat(pc.sh.dwords[70]), uintBitsToFloat(pc.sh.dwords[71]));
  const vec2 offset = vec2(uintBitsToFloat(pc.sh.dwords[66]), uintBitsToFloat(pc.sh.dwords[67]));
  const vec4 p = vec4(i_pos.xyz, 1.0);
  gl_Position = vec4(p.xy * scale + offset * p.w, p.z, p.w);
  for (int k = 0; k < 18; ++k) o[k] = vec4(0.0);
  o[0] = i_color;
  o[16] = i_color;
}
