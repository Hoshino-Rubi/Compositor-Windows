#pragma once
namespace comp {
// The LUT contains every native channel value (256 or 65536), generated with
// the reference CPU adjustment. Pixels, alpha handling and coverage run on GPU.
inline constexpr char gpuToneShader[] = R"hlsl(
Texture2D<float4> backdrop : register(t0);
Texture2D<uint4> toneTable : register(t1);
Texture2D<float4> coverage : register(t2);
Texture2D<float> alphaFactors : register(t3);
RWTexture2D<float4> outputImage : register(u0);
cbuffer Settings : register(b0) {
    uint width; uint height; uint unused; float maximum;
    uint offsetX; uint offsetY; uint firstLayer; uint opacityBits;
};
uint lookup(uint value, uint channel) {
    return toneTable.Load(int3(value & 255, value >> 8, 0))[channel];
}
[numthreads(16,16,1)]
void main(uint3 p:SV_DispatchThreadID) {
    if(p.x>=width || p.y>=height) return;
    p.xy+=uint2(offsetX,offsetY);
    float4 before=firstLayer ? float4(0,0,0,0) : backdrop.Load(int3(p.xy,0));
    uint4 native=(uint4)floor(saturate(before)*maximum+.5);
    uint3 straight=uint3(0,0,0);
    if(native.a>0) {
        precise float3 scaled=float3(native.rgb)*alphaFactors.Load(int3(native.a & 255, native.a >> 8, 0));
        straight=(uint3)round(scaled);
    }
    straight=min(straight,(uint)maximum);
    uint3 mapped=uint3(lookup(straight.r,0),lookup(straight.g,1),lookup(straight.b,2));
    uint shift=maximum==255 ? 8 : 16;
    uint3 product=mapped*native.a;
    uint3 premultiplied=(product+(product>>shift)+(1u<<(shift-1)))>>shift;
    float4 after=float4(premultiplied,native.a)/maximum;
    float amount=coverage.Load(int3(p.xy,0)).a*asfloat(opacityBits);
    outputImage[p.xy]=floor(saturate(before*(1-amount)+after*amount)*maximum+.5)/maximum;
}
)hlsl";
} // namespace comp
