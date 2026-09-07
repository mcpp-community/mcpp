#version 450

// The interpolated colour, and nothing else. A fragment shader that ignored its
// input and wrote a constant would produce an image with one non-zero channel
// at the centre; the assertion in `main.cpp` is written to tell those apart.
layout(location = 0) in  vec3 v_color;
layout(location = 0) out vec4 o_color;

void main() {
    o_color = vec4(v_color, 1.0);
}
