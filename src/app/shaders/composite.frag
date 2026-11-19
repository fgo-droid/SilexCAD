#version 440
// Background gradient + FXAA on the opaque layer + OIT resolve.
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Buf {
    vec4 bgTop;
    vec4 bgBottom;
    vec4 info;  // x,y = size in pixels, z = 1 if framebuffer is y-up
} ubuf;
layout(binding = 1) uniform sampler2D opaqueTex;
layout(binding = 2) uniform sampler2D accumTex;
layout(binding = 3) uniform sampler2D revealTex;

vec3 background(vec2 uv)
{
    float t = ubuf.info.z > 0.5 ? uv.y : 1.0 - uv.y;
    return mix(ubuf.bgBottom.rgb, ubuf.bgTop.rgb, t);
}

vec3 scene(vec2 uv)
{
    vec4 c = texture(opaqueTex, uv);
    return c.rgb + background(uv) * (1.0 - c.a);
}

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

vec3 fxaa(vec2 uv, vec2 rcp)
{
    vec3 nw = scene(uv + vec2(-1.0, -1.0) * rcp);
    vec3 ne = scene(uv + vec2(1.0, -1.0) * rcp);
    vec3 sw = scene(uv + vec2(-1.0, 1.0) * rcp);
    vec3 se = scene(uv + vec2(1.0, 1.0) * rcp);
    vec3 m = scene(uv);
    float lnw = luma(nw), lne = luma(ne), lsw = luma(sw), lse = luma(se), lm = luma(m);
    float lmin = min(lm, min(min(lnw, lne), min(lsw, lse)));
    float lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));
    if (lmax - lmin < max(0.0312, lmax * 0.125))
        return m;
    vec2 dir = vec2(-((lnw + lne) - (lsw + lse)), ((lnw + lsw) - (lne + lse)));
    float reduce = max((lnw + lne + lsw + lse) * 0.25 * (1.0 / 8.0), 1.0 / 128.0);
    float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcpMin, vec2(-8.0), vec2(8.0)) * rcp;
    vec3 a = 0.5 * (scene(uv + dir * (1.0 / 3.0 - 0.5)) + scene(uv + dir * (2.0 / 3.0 - 0.5)));
    vec3 b = a * 0.5 + 0.25 * (scene(uv + dir * -0.5) + scene(uv + dir * 0.5));
    float lb = luma(b);
    return (lb < lmin || lb > lmax) ? a : b;
}

void main()
{
    vec2 size = ubuf.info.xy;
    vec2 uv = gl_FragCoord.xy / size;
    vec3 base = fxaa(uv, 1.0 / size);
    vec4 acc = texture(accumTex, uv);
    float q = clamp(texture(revealTex, uv).r, 0.0, 1.0);
    vec3 avg = acc.rgb / max(acc.a, 1e-5);
    fragColor = vec4(mix(base, avg, q), 1.0);
}
