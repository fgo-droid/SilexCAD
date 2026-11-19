#version 440
// Shared by the opaque (mesh.frag) and OIT (oit.frag) passes.
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec4 color;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec3 vWorld;

layout(std140, binding = 0) uniform Buf {
    mat4 mvp;
    vec4 eye;     // xyz = camera position, w = 1 for orthographic
    vec4 params;  // x = alpha multiplier, y = depth bias, z = view dir x (ortho), w unused
    vec4 viewDir; // xyz = camera forward (used in orthographic mode)
} ubuf;

void main()
{
    vNormal = normal;
    vColor = color;
    vWorld = position;
    gl_Position = ubuf.mvp * vec4(position, 1.0);
}
