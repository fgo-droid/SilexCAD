#version 440
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec3 vWorld;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Buf {
    mat4 mvp;
    vec4 eye;
    vec4 params;
    vec4 viewDir;
    vec4 clip;
} ubuf;

void main()
{
    if (dot(ubuf.clip.xyz, ubuf.clip.xyz) > 0.0 && dot(ubuf.clip.xyz, vWorld) > ubuf.clip.w)
        discard;  // section view
    fragColor = vec4(vColor.rgb * vColor.a, vColor.a);  // premultiplied
}
