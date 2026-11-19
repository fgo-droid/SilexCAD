#version 440
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec3 vWorld;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Buf {
    mat4 mvp;
    vec4 eye;
    vec4 params;
    vec4 viewDir;
} ubuf;

vec3 shade(vec3 base, vec3 n, vec3 v)
{
    if (dot(n, v) < 0.0)
        n = -n;  // two-sided lighting
    float head = max(dot(n, v), 0.0);
    vec3 key = normalize(vec3(0.4, 0.3, 1.0));
    float k = max(dot(n, key), 0.0);
    float fill = max(dot(n, normalize(vec3(-0.6, -0.4, 0.3))), 0.0);
    vec3 h = normalize(v + key);
    float spec = pow(max(dot(n, h), 0.0), 48.0);
    vec3 c = base * (0.30 + 0.45 * head + 0.25 * k + 0.08 * fill) + vec3(0.10) * spec;
    return c;
}

vec3 viewVector()
{
    return ubuf.eye.w > 0.5 ? -ubuf.viewDir.xyz : normalize(ubuf.eye.xyz - vWorld);
}

void main()
{
    vec3 c = shade(vColor.rgb, normalize(vNormal), viewVector());
    fragColor = vec4(c, 1.0);
}
