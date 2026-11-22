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
    vec4 clip;
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
    if (dot(ubuf.clip.xyz, ubuf.clip.xyz) > 0.0) {
        if (dot(ubuf.clip.xyz, vWorld) > ubuf.clip.w)
            discard;  // section view: material beyond the cutting plane is removed
        if (!gl_FrontFacing) {
            // Seen from inside a closed solid = cut material: solid cap colour with a light hatch.
            float h = mod(gl_FragCoord.x + gl_FragCoord.y, 9.0) < 1.2 ? 0.82 : 1.0;
            fragColor = vec4(vec3(0.93, 0.42, 0.24) * h, 1.0);
            return;
        }
    }
    vec3 c = shade(vColor.rgb, normalize(vNormal), viewVector());
    fragColor = vec4(c, 1.0);
}
