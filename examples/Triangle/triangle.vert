#version 450
layout(location = 0) out vec3 color;
const vec2 positions[3] = vec2[](vec2(0.0, -0.8), vec2(0.8, 0.8), vec2(-0.8, 0.8));
void       main()
{
    gl_Position = vec4(positions[gl_VertexIndex], 0, 1);
    color       = vec3(gl_VertexIndex == 0, gl_VertexIndex == 1, gl_VertexIndex == 2);
}
