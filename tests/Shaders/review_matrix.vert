#version 450
layout(location = 0) in mat4 transform;
void main()
{
    gl_Position = transform * vec4(0, 0, 0, 1);
}
