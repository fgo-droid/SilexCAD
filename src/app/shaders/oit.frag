#version 440
// Weighted blended order-independent transparency (McGuire & Bavoil 2013).
// Target 0: premultiplied colour accumulation (additive), target 1: coverage q = 1 - prod(1 - a).
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec3 vWorld;
layout(location = 0) out vec4 accum;
layout(location = 1) out vec4 reveal;

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
    float a = clamp(vColor.a * ubuf.params.x, 0.0, 1.0);
    float z = gl_FragCoord.z;
    float w = clamp(pow(min(1.0, a * 10.0) + 0.01, 3.0) * 1e8 * pow(1.0 - z * 0.9, 3.0), 1e-2, 3e3);
    accum = vec4(c * a, a) * w;
    reveal = vec4(a);
}
