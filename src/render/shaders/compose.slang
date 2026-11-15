// Scene target to display: expands the scene (or its temporal result), applies exposure, bloom and the post stack, and
// tonemaps into the frame before lines and overlays draw over it.

#include "scene_color.slang"
#include "tonemap.slang"

struct ComposePush
{
    float4 params; // x: display gamma, y: 1 to dither, z: exposure, w: tonemapper (0 neutral, 1 AgX, 2 ACES, 3 linear)
    float4 grade;  // x: contrast, y: saturation, z: 1 for the post stack, w: AgX look (0 none, 1 punchy, 2 golden)
    float4 white;  // rgb: white balance gains, w: LUT intensity (0 skips it)
    float4 lift;   // rgb, w: vignette
    float4 gammaC; // rgb, w: film grain
    float4 gain;   // rgb, w: grain seed
    float4 lens;   // x: chromatic aberration (pixels at the corner), y: lens distortion, z: sharpen, w: bloom intensity
    float4 size;   // extent, then its inverse
    float4 misc;   // x: 1 to apply the auto exposure, y: bloom scale (one over its levels)
};

[[vk::push_constant]] ComposePush cp;
[[vk::binding(0, 0)]] Sampler2D composeTex; // compressed scene, linear filtering
[[vk::binding(1, 0)]] Sampler2D bloomTex;   // half resolution, expanded radiance
[[vk::binding(2, 0)]] Sampler3D lutTex;     // display-encoded in and out
[[vk::binding(3, 0)]] Sampler2D exposureTex; // exposure.slang's state: texel 0 is the multiplier

struct ComposeOut
{
    float4 pos : SV_Position;
};

[shader("vertex")]
ComposeOut composeVS(uint vid: SV_VertexID)
{
    float2 uv = float2((vid << 1) & 2, vid & 2);
    ComposeOut o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

float3 toSrgb(float3 c)
{
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055);
}

float3 fromSrgb(float3 c)
{
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

// Half an 8-bit step of noise, applied in sRGB space since that is where the swapchain quantizes.
float3 dither(float3 linear, float2 pixel)
{
    float noise = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715)))) - 0.5;
    float3 srgb = saturate(toSrgb(linear) + noise / 255.0);
    return fromSrgb(srgb);
}

float3 sceneAt(float2 uv)
{
    return sceneExpand(composeTex.SampleLevel(uv, 0).rgb);
}

float hash(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

[shader("fragment")]
float4 composePS(ComposeOut i) : SV_Target
{
    const bool stack = cp.grade.z > 0.5;
    float2 uv = i.pos.xy * cp.size.zw;
    const float alpha = composeTex.Load(int3(int2(i.pos.xy), 0)).a;
    float3 c;
    if (!stack)
        c = sceneExpand(composeTex.Load(int3(int2(i.pos.xy), 0)).rgb);
    else
    {
        // Barrel or pincushion distortion around the centre, then a radial colour split.
        float2 centred = uv * 2.0 - 1.0;
        float aspect = cp.size.x * cp.size.w;
        float2 d = centred * float2(aspect, 1.0);
        float r2 = dot(d, d) / (aspect * aspect + 1.0);
        float2 warped = centred * (1.0 + cp.lens.y * r2);
        float2 base = warped * 0.5 + 0.5;
        if (cp.lens.x > 0.0)
        {
            float2 shift = warped * cp.lens.x * cp.size.zw;
            c = float3(sceneAt(base + shift).r, sceneAt(base).g, sceneAt(base - shift).b);
        }
        else
            c = sceneAt(base);
        if (cp.lens.z > 0.0)
        {
            // Unsharp mask on the four neighbours.
            float3 n = sceneAt(base + float2(cp.size.z, 0)) + sceneAt(base - float2(cp.size.z, 0)) + sceneAt(base + float2(0, cp.size.w)) +
                       sceneAt(base - float2(0, cp.size.w));
            c = max(c + cp.lens.z * (c - 0.25 * n), 0.0);
        }
        if (cp.lens.w > 0.0)
            c = lerp(c, bloomTex.SampleLevel(base, 0).rgb * cp.misc.y, cp.lens.w);
        uv = base;
    }

    float exposure = cp.params.z;
    if (cp.misc.x > 0.5)
        exposure *= exposureTex.Load(int3(0, 0, 0)).r;
    c *= exposure;

    if (stack)
    {
        c *= cp.white.rgb;
        // Contrast around 18% grey in log space, then saturation around the pixel's luminance.
        const float grey = 0.18;
        c = grey * pow(max(c / grey, 0.0), cp.grade.x);
        float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
        c = max(luma + cp.grade.y * (c - luma), 0.0);
    }

    c = tonemap(c, uint(cp.params.w + 0.5), uint(cp.grade.w + 0.5));
    c = pow(max(c, 0.0), 1.0 / max(cp.params.x, 0.01));

    if (stack)
    {
        // Lift, gamma, gain on the display-encoded picture, as in a grading suite.
        float3 s = toSrgb(saturate(c));
        s = saturate(s * cp.gain.rgb + cp.lift.rgb * (1.0 - s));
        s = pow(s, 1.0 / max(cp.gammaC.rgb, 0.01));
        if (cp.white.w > 0.0)
        {
            float3 dims;
            lutTex.GetDimensions(dims.x, dims.y, dims.z);
            float3 coord = s * ((dims - 1.0) / dims) + 0.5 / dims;
            s = lerp(s, lutTex.SampleLevel(coord, 0).rgb, cp.white.w);
        }
        if (cp.lift.w > 0.0)
        {
            float2 v = (uv - 0.5) * float2(cp.size.x * cp.size.w, 1.0);
            s *= 1.0 - cp.lift.w * smoothstep(0.25, 1.0, dot(v, v) * 1.6);
        }
        if (cp.gammaC.w > 0.0)
        {
            // Grain strongest in the mid-tones, as on film.
            float n = hash(i.pos.xy + cp.gain.w * 97.0) + hash(i.pos.xy * 1.37 + cp.gain.w * 61.0) - 1.0;
            float luma = dot(s, float3(0.2126, 0.7152, 0.0722));
            s = saturate(s + n * cp.gammaC.w * 0.12 * (1.0 - abs(luma * 2.0 - 1.0) * 0.6));
        }
        c = fromSrgb(s);
    }
    if (cp.params.y > 0.5)
        c = dither(c, i.pos.xy);
    return float4(c, alpha);
}
