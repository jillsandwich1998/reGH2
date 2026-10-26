#version 450
// The on-screen menu (Dear ImGui draw lists recorded by the application): vertices in the UI's own coordinate space, mapped to clip space by the two scale factors in the push constants.
layout(push_constant) uniform Constants { vec2 clip_scale; } pc;   // 2 / width, 2 / height
layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;
void main() {
  gl_Position = vec4(in_position * pc.clip_scale - 1.0, 0.0, 1.0);
  out_uv = in_uv;
  out_color = in_color;
}
