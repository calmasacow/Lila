// The path tracer's extended shading channels (render::Material's sheen, coat, anisotropy, subsurface, transmission,
// specular colour and IOR): their maps applied to a hit, and the lobes they add to the BSDF.

void applyExtendedMaps(PtSection s, inout Hit h) {}

float3 extendedLobes(Hit h, float3 n, float3 v, float3 l, float3 hv, float nl, float nv, float nh, float vh, float3 diff)
{
    return float3(0.0);
}
