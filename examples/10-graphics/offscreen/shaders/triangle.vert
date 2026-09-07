#version 450

// No vertex buffer, no vertex input state: the three positions are indexed by
// `gl_VertexIndex`. That is deliberate, and it is what makes the pixel
// assertion in `main.cpp` a statement about the SHADERS rather than about a
// buffer upload -- there is no vertex data for a bug to hide in.
layout(location = 0) out vec3 v_color;

vec2 positions[3] = vec2[](
    vec2( 0.0, -0.5),
    vec2( 0.5,  0.5),
    vec2(-0.5,  0.5));

vec3 colors[3] = vec3[](
    vec3(1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0),
    vec3(0.0, 0.0, 1.0));

void main() {
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    v_color     = colors[gl_VertexIndex];
}
