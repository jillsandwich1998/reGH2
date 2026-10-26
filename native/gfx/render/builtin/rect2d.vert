#version 450
// The D3D library's own two-component rectangle shader (15 dwords, 0x8206B158): POSITION0 (2 floats) -> (x, y, 0, 1). No interpolators are written.
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(buffer_reference, std430) readonly buffer Shared { uint dwords[]; };
layout(push_constant) uniform PC { uint64_t vs; uint64_t ps; Shared sh; } pc;
layout(location = 0) in vec4 i_pos;
layout(location = 0) out vec4 o[18];
void main() {
  const vec2 scale = vec2(uintBitsToFloat(pc.sh.dwords[70]), uintBitsToFloat(pc.sh.dwords[71]));
  const vec2 offset = vec2(uintBitsToFloat(pc.sh.dwords[66]), uintBitsToFloat(pc.sh.dwords[67]));
  const vec4 p = vec4(i_pos.xy, 0.0, 1.0);
  gl_Position = vec4(p.xy * scale + offset * p.w, p.z, p.w);
  for (int k = 0; k < 18; ++k) o[k] = vec4(0.0);
}
