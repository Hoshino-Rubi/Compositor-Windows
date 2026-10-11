#pragma once
namespace comp {
// Three horizontal/vertical integer box passes match blurImage's edge clamping
// and truncation at every pass. A 256-pixel block plus both halos fits 1024 slots.
inline constexpr char gpuBlurShader[] = R"hlsl(
Texture2D<float4> inputImage : register(t0);
RWTexture2D<float4> outputImage : register(u0);
cbuffer Settings : register(b0) {
    uint width; uint height; uint axis; float maximum;
    uint radius; uint unused; uint firstLayer; uint padding;
};
groupshared uint4 prefixA[1024];
groupshared uint4 prefixB[1024];
[numthreads(256,1,1)]
void main(uint3 group:SV_GroupID, uint index:SV_GroupIndex) {
    uint length=axis==0 ? width : height;
    uint start=group.x*256;
    for(uint i=index;i<1024;i+=256) {
        uint location=(uint)clamp((int)(start+i)-(int)radius,0,(int)length-1);
        uint2 pixel=axis==0 ? uint2(location,group.y) : uint2(group.y,location);
        prefixA[i]=i<256+2*radius ? (uint4)floor(saturate(inputImage.Load(int3(pixel,0)))*maximum+.5) : uint4(0,0,0,0);
    }
    GroupMemoryBarrierWithGroupSync();
    // Uniform barriers: even threads beyond the edge participate in the scan.
    [loop] for(uint step=1;step<1024;step*=4) {
        for(uint i=index;i<1024;i+=256)
            prefixB[i]=prefixA[i]+(i>=step ? prefixA[(i-step)&1023] : uint4(0,0,0,0));
        GroupMemoryBarrierWithGroupSync();
        for(uint j=index;j<1024;j+=256)
            prefixA[j]=prefixB[j]+(j>=step*2 ? prefixB[(j-step*2)&1023] : uint4(0,0,0,0));
        GroupMemoryBarrierWithGroupSync();
    }
    if(start+index>=length) return;
    uint4 sum=prefixA[index+2*radius]-(index>0 ? prefixA[(index-1)&1023] : uint4(0,0,0,0));
    uint2 target=axis==0 ? uint2(start+index,group.y) : uint2(group.y,start+index);
    outputImage[target]=float4(sum/(2*radius+1))/maximum;
}
)hlsl";
inline constexpr char gpuBlurMixShader[] = R"hlsl(
Texture2D<float4> backdrop : register(t0);
Texture2D<float4> adjusted : register(t1);
Texture2D<float4> coverage : register(t2);
RWTexture2D<float4> outputImage : register(u0);
cbuffer Settings : register(b0) {
    uint width; uint height; uint mode; float maximum;
    uint offsetX; uint offsetY; uint firstLayer; uint opacityBits;
};
// ADJUSTMENT_BLEND
[numthreads(16,16,1)]
void main(uint3 p:SV_DispatchThreadID) {
    if(p.x>=width || p.y>=height) return;
    p.xy+=uint2(offsetX,offsetY);
    float4 before=backdrop.Load(int3(p.xy,0)), after=adjusted.Load(int3(p.xy,0));
    after=blendAdjusted(before,after);
    float amount=coverage.Load(int3(p.xy,0)).a*asfloat(opacityBits);
    outputImage[p.xy]=floor(saturate(before*(1-amount)+after*amount)*maximum+.5)/maximum;
}
)hlsl";
} // namespace comp
