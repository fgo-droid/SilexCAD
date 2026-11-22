#version 440
layout(location = 0) in vec3 position;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 vColor;
layout(location = 1) out vec3 vWorld;

layout(std140, binding = 0) uniform Buf {
    mat4 mvp;
    vec4 eye;
    vec4 params;  // y = depth bias (pulls edges in front of their faces)
    vec4 viewDir;
    vec4 clip;
} ubuf;

void main()
{
    vColor = color;
    vWorld = position;
    vec4 p = ubuf.mvp * vec4(position, 1.0);
    p.z -= ubuf.params.y * p.w;
    gl_Position = p;
}
