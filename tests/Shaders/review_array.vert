#version 450
layout(location = 0) in vec4 offsets[2];
void main()
{
    gl_Position = offsets[0] + offsets[1];
}
