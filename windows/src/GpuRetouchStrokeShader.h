#pragma once
namespace comp {
inline constexpr char gpuRetouchStrokeShader[] = R"hlsl(
Texture2D<float4> original : register(t0);
Texture2D<float2> oldField : register(t1);
Texture2D<float> coverage : register(t2);
RWStructuredBuffer<float4> canvas : register(u0);
RWTexture2D<float2> field : register(u1);
RWStructuredBuffer<float4> carried : register(u2);
RWTexture2D<uint4> packed : register(u3);
cbuffer Settings : register(b0) {
    int2 size; float maximum; uint hasMask;
    int2 origin; int2 extent;
    float2 center; float2 move;
    float radius; float hardness; float strength; int r;
};
float weight(float distance) {
    float u = distance / radius;
    if (u >= 1) return 0;
    float t = u <= hardness ? 1 : (1-u)/(1-hardness);
    return t*t*(3-2*t);
}
float4 colorAt(int2 p) { return floor(original.Load(int3(clamp(p,0,size-1),0))*maximum+.5); }
// Separate integer coordinates from fractional displacement. Adding a tiny
// displacement to a large float coordinate would discard 16-bit image detail.
void coordinates(int2 p,float2 delta,out int2 b,out float2 f) {
    float2 whole=floor(delta); b=p+int2(whole); f=delta-whole;
    if(b.x<0||b.x>=size.x-1)f.x=0;
    if(b.y<0||b.y>=size.y-1)f.y=0;
    b=clamp(b,0,size-1);
}
float4 sampleOriginal(int2 p,float2 delta) {
    int2 b;float2 f;coordinates(p,delta,b,f);
    return lerp(lerp(colorAt(b),colorAt(b+int2(1,0)),f.x),lerp(colorAt(b+int2(0,1)),colorAt(b+1),f.x),f.y);
}
float2 sampleField(int2 p,float2 delta) {
    int2 b;float2 f;coordinates(p,delta,b,f);
    return lerp(lerp(oldField.Load(int3(b,0)),oldField.Load(int3(min(b+int2(1,0),size-1),0)),f.x),
                lerp(oldField.Load(int3(min(b+int2(0,1),size-1),0)),oldField.Load(int3(min(b+1,size-1),0)),f.x),f.y);
}
float4 quantize(float4 v) { v=clamp(floor(v+.5),0,maximum); v.rgb=min(v.rgb,v.a); return v; }
[numthreads(16,16,1)]
void initialize(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy >= size)) return;
    canvas[id.y*size.x+id.x]=colorAt(int2(id.xy)); field[id.xy]=0;
}
[numthreads(16,16,1)]
void pickup(uint3 id:SV_DispatchThreadID) {
    int side=2*r+1; if(any(id.xy>=side))return;
    int2 p=int2(sign(center)*floor(abs(center)+.5))+int2(id.xy)-r;
    carried[id.y*side+id.x]=all(p>=0)&&all(p<size)?canvas[p.y*size.x+p.x]:0;
}
[numthreads(16,16,1)]
void liquify(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=extent))return; int2 p=origin+int2(id.xy);
    float w=weight(length(float2(id.xy)+.5-center));
    if(hasMask)w*=coverage.Load(int3(p,0)); if(w<=0)return;
    precise float2 moved=sampleField(p,-move*w)-move*w;
    field[p]=moved; canvas[p.y*size.x+p.x]=quantize(sampleOriginal(p,moved));
}
[numthreads(16,16,1)]
void smudge(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=extent))return; int2 p=origin+int2(id.xy), delta=p-int2(sign(center)*floor(abs(center)+.5));
    float w=weight(length(float2(delta)));
    if(hasMask)w*=coverage.Load(int3(p,0)); if(w<=0)return;
    int index=(delta.y+r)*(2*r+1)+delta.x+r;
    float4 painted=lerp(canvas[p.y*size.x+p.x],carried[index],w*strength);
    carried[index]=painted; canvas[p.y*size.x+p.x]=quantize(painted);
}
[numthreads(16,16,1)]
void packResult(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=extent))return; int2 p=origin+int2(id.xy);
    uint4 v=(uint4)quantize(canvas[p.y*size.x+p.x]); packed[id.xy]=maximum==255?v.bgra:v;
}
)hlsl";
}
