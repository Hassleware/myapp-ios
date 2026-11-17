// Polished river stones: the standard look plus a sun glint off each stone's dome (from the normal map), so wet,
// smooth pebbles catch the light. Gaps are dark and dull: the glint is scaled by how bright the texture is.

#include "common.slang"

[shader("vertex")]
VSOut vsMain(VSIn input)
{
    return standardVertex(input);
}

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target
{
    float4 albedo = tex0.Sample(input.uv) * input.color;
    if (albedo.a < push.flags.z)
        discard;
    float3 n = surfaceNormal(input);
    float3 sunDir = -normalize(frame.lightDir.xyz);
    float3 v = normalize(frame.cameraPos.xyz - input.world);
    float3 h = normalize(sunDir + v);
    float shine = pow(saturate(dot(n, h)), 30.0) * 0.3 * saturate(albedo.g * 4.0) * sunShadow(input.world, n);
    float3 c = albedo.rgb * lightAt(input.world, n) + frame.lightColor.rgb * shine;
    return float4(applyFog(c, input.world), 1.0);
}
