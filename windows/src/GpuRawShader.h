#pragma once
namespace comp {
inline constexpr char gpuRawShader[] = R"hlsl(
RWByteAddressBuffer rawBins : register(u1);
[numthreads(16,16,1)]
void rawCrop(uint3 tid:SV_DispatchThreadID){if(any(tid.xy>=dimensions))return;result[tid.xy]=source.Load(int3(int2(tid.xy)+int2(P[1].xy),0));}
float decodeSrgb(float e){return e<=.04045?e/12.92:pow((e+.055)/1.055,2.4);}
float3 relight(float3 c,float target) {
    target=saturate(target);float y=luminance709(c);
    if(abs(target-y)<1e-8)return c;
    return y<1e-8?(target>y?target.xxx:c):saturate(c*(target/y));
}
float3 sampleCube(Texture2D<float4> table,float3 rgb,int n,int offset=0) {
    float3 v=saturate(rgb)*(n-1);int3 base=min((int3)v,n-2);float3 f=v-base;
    float3 outColor=0;
    for(int r=0;r<2;++r)for(int g=0;g<2;++g)for(int b=0;b<2;++b)
        outColor+=table.Load(int3((base.g+g)*n+base.b+b,offset+base.r+r,0)).rgb*(r?f.r:1-f.r)*(g?f.g:1-f.g)*(b?f.b:1-f.b);
    return outColor;
}
groupshared float4 rawReduction[256];
[numthreads(16,16,1)]
void rawStatistics(uint3 tid:SV_DispatchThreadID,uint3 group:SV_GroupID,uint index:SV_GroupIndex) {
    float4 p=source.Load(int3(tid.xy,0));float3 c=straight(p);int mode=(int)P[1].z;float4 value=0;
    if(mode==2)value=p.a>0?float4(tid.xy,tid.xy):float4(P[1].xy,-1,-1);
    else if(mode==1)value=floor(luminance709(c)*255+.5)>=P[1].w?p:float4(0,0,0,0);
    else {float3 linearColor=float3(decodeSrgb(c.r),decodeSrgb(c.g),decodeSrgb(c.b));float hi=max(linearColor.r,max(linearColor.g,linearColor.b));value=float4(hi,luminance709(linearColor),log(hi+3e-4),1)*p.a;}
    rawReduction[index]=value;GroupMemoryBarrierWithGroupSync();
    for(uint stride=128;stride>0;stride/=2){if(index<stride){if(mode==2)rawReduction[index]=float4(min(rawReduction[index].xy,rawReduction[index+stride].xy),max(rawReduction[index].zw,rawReduction[index+stride].zw));else rawReduction[index]+=rawReduction[index+stride];}GroupMemoryBarrierWithGroupSync();}
    if(index==0)result[group.xy]=rawReduction[0];
}
groupshared uint histogramBins[256];
[numthreads(16,16,1)]
void rawHistogram(uint3 tid:SV_DispatchThreadID,uint index:SV_GroupIndex) {
    histogramBins[index]=0;GroupMemoryBarrierWithGroupSync();
    float4 p=source.Load(int3(tid.xy,0));uint bin=(uint)floor(luminance709(straight(p))*255+.5);
    InterlockedAdd(histogramBins[bin],(uint)floor(p.a*maximum+.5));GroupMemoryBarrierWithGroupSync();
    uint weight=histogramBins[index];if(weight){uint old;rawBins.InterlockedAdd(index*8,weight,old);if(old+weight<old)rawBins.InterlockedAdd(index*8+4,1);}
}
[numthreads(16,16,1)]
void rawTable(uint3 tid:SV_DispatchThreadID) {
    if(any(tid.xy>=dimensions))return;float4 p=source.Load(int3(tid.xy,0));float3 c=sampleCube(auxiliary,straight(p),(int)P[1].x);
    if(P[1].y==1)c=step((maximum-.5)/maximum,c);
    if(P[1].y==2)c=any(c<=.5/maximum)?float3(c.r<=.5/maximum?0:1,c.g<=.5/maximum?0:1,c.b<=.5/maximum?0:1):float3(1,1,1);
    result[tid.xy]=writeColor(c,p.a);
}
[numthreads(16,16,1)]
void rawCalibration(uint3 tid:SV_DispatchThreadID) {
    if(any(tid.xy>=dimensions))return;float4 p=source.Load(int3(tid.xy,0));float3 h=rgbHsl(straight(p));
    float version=P[1].y<=1?.55:P[1].y==2?.65:P[1].y==3?.75:P[1].y==4?.85:P[1].y==5?.92:1;
    bool shadow=h.z<.35;
#ifdef PRECISE_PREDICATES
    double3 native=double3(floor(p.rgb*maximum+.5))/max(1.0,double(floor(p.a*maximum+.5)));
    shadow=(max(native.r,max(native.g,native.b))+min(native.r,min(native.g,native.b)))*.5<.35;
#endif
    if(shadow)h.x=frac(h.x+P[1].x*.0006*version+1);
    result[tid.xy]=writeColor(hslRgb(h),p.a);
}
float curveSample(float x,int channel) {
    float v=saturate(x)*255;int i=min(254,(int)v);
    return lerp(fourth.Load(int3(i,0,0))[channel],fourth.Load(int3(i+1,0,0))[channel],v-i);
}
float pointValue(int pointIndex,int component){int i=16+pointIndex*9+component;return P[i/4][i%4];}
float pointWeight(float3 hsl,int pointIndex) {
    float d=abs(hsl.x-pointValue(pointIndex,0));d=min(d,1-d);
    float3 halfRange=max(float3(pointValue(pointIndex,6),pointValue(pointIndex,7),pointValue(pointIndex,8)),.01);
    float3 w=1-float3(d,abs(hsl.y-pointValue(pointIndex,1)),abs(hsl.z-pointValue(pointIndex,2)))/halfRange;
    return any(w<0)?0:w.x*w.y*w.z;
}
[numthreads(16,16,1)]
void rawCurveColor(uint3 tid:SV_DispatchThreadID) {
    if(any(tid.xy>=dimensions))return;float4 p=source.Load(int3(tid.xy,0));float3 c=straight(p);
    float3 curved=float3(curveSample(c.r,0),curveSample(c.g,0),curveSample(c.b,0));
    if(P[1].x<0)curved=lerp(curved,relight(c,curveSample(luminance709(c),0)),-P[1].x);
    else if(P[1].x>0){float lum=luminance709(curved);curved=saturate(lum+(curved-lum)*(1+P[1].x));}
    c=float3(curveSample(curved.r,1),curveSample(curved.g,2),curveSample(curved.b,3));
    c=sampleCube(auxiliary,c,(int)P[1].w);float3 h=rgbHsl(c),original=h;
    for(int i=0;i<(int)P[1].y;++i){float weight=pointWeight(h,i);h.x+=pointValue(i,3)*weight*(30./360);h.y=saturate(h.y*(1+pointValue(i,4)*weight));h.z=saturate(h.z+pointValue(i,5)*weight*.25);}
    h.x=frac(h.x+1);c=sampleCube(third,hslRgb(h),(int)P[1].w);
    if(P[1].z>=0&&P[1].z<P[1].y&&pointWeight(original,(int)P[1].z)<=.05)c*=.35;
    result[tid.xy]=writeColor(c,p.a);
}
[numthreads(16,16,1)]
void rawLuma(uint3 tid:SV_DispatchThreadID){if(any(tid.xy>=dimensions))return;float l=luminance709(straight(source.Load(int3(tid.xy,0))));result[tid.xy]=l.xxxx;}
[numthreads(16,16,1)]
void rawBox(uint3 tid:SV_DispatchThreadID) {
    if(any(tid.xy>=dimensions))return;float4 sum=0;int radius=(int)P[1].x;int2 axis=P[1].y>0?int2(0,1):int2(1,0);
    for(int k=-radius;k<=radius;++k)sum+=loadClamped(int2(tid.xy)+axis*k);
    result[tid.xy]=sum/(2*radius+1);
}
[numthreads(16,16,1)]
void rawSquare(uint3 tid:SV_DispatchThreadID){if(any(tid.xy>=dimensions))return;float4 v=source.Load(int3(tid.xy,0));result[tid.xy]=v*v;}
[numthreads(16,16,1)]
void rawGuidedCoefficients(uint3 tid:SV_DispatchThreadID) {
    if(any(tid.xy>=dimensions))return;float m=source.Load(int3(tid.xy,0)).r,v=auxiliary.Load(int3(tid.xy,0)).r-m*m;
    float a=v/(v+.01);result[tid.xy]=float4(a,m-a*m,0,0);
}
[numthreads(16,16,1)]
void rawGuidedFinish(uint3 tid:SV_DispatchThreadID) {
    if(any(tid.xy>=dimensions))return;float l=source.Load(int3(tid.xy,0)).r;float2 ab=auxiliary.Load(int3(tid.xy,0)).rg;
    result[tid.xy]=(ab.x*l+ab.y).xxxx;
}
[numthreads(16,16,1)]
void rawGlowThreshold(uint3 tid:SV_DispatchThreadID){if(any(tid.xy>=dimensions))return;float l=source.Load(int3(tid.xy,0)).r;result[tid.xy]=saturate((l-P[1].x)/max(.05,1-P[1].x)).xxxx;}
float3 rawVignette(float3 c,int2 xy) {
    float amount=P[5].x,midpoint=P[5].y,roundness=P[5].z,feather=P[5].w,highlights=P[6].x;int style=(int)P[6].y;
    if(amount==0)return c;float mask=vignetteMask(float2(xy)+.5,midpoint+5.9,min(100,100+roundness),feather),effect=amount/100*mask;
    if(style==0&&mask>0) {
        float3 linearColor=float3(decodeSrgb(c.r),decodeSrgb(c.g),decodeSrgb(c.b));float bright=saturate((luminance709(c)-.45)/.55);
        if(amount<0)linearColor*=exp(-5.8*pow(-amount/100,1.43)*mask*(1-.3*bright)*(1-highlights/100*bright));
        else {float a=amount/100;linearColor+=(1-linearColor)*pow(a,.85)*pow(mask,1+1.5*(1-a));}
        c=float3(linearColor.r<=.0031308?linearColor.r*12.92:1.055*pow(linearColor.r,1/2.4)-.055,linearColor.g<=.0031308?linearColor.g*12.92:1.055*pow(linearColor.g,1/2.4)-.055,linearColor.b<=.0031308?linearColor.b*12.92:1.055*pow(linearColor.b,1/2.4)-.055);return saturate(c);
    }
    c=effect<0?c*(1+effect):c+(1-c)*effect;
    if(style==1&&mask>0)c=saturate(luminance709(c)+(c-luminance709(c))*(1-.75*mask*abs(amount/100)));
    return c;
}
[numthreads(16,16,1)]
void rawEffects(uint3 tid:SV_DispatchThreadID) {
    if(any(tid.xy>=dimensions))return;float4 p=source.Load(int3(tid.xy,0));float3 c=straight(p);float l=luminance709(c),detail=0;
    if(P[1].x!=0)detail+=P[1].x/100*(P[1].x>0?.6:.5)*(l-auxiliary.Load(int3(tid.xy,0)).r);
    if(P[1].y!=0)detail+=P[1].y/100*(P[1].y>0?.85:.5)*(l-third.Load(int3(tid.xy,0)).r);
    if(detail!=0)c=relight(c,l+detail);
    float d=P[1].z/100;if(d<0)c=saturate(lerp(c,P[2].xyz,.3*pow(-d,1.3)));
    else if(d>0){float3 ratio=c/max(P[2].xyz,.001);float dark=min(1,min(ratio.r,min(ratio.g,ratio.b)));c=saturate((c-P[2].xyz)/max(.5,1-.7*d*dark)+P[2].xyz);}
    if(P[1].w>0)c=saturate(c+fourth.Load(int3(tid.xy,0)).r*(P[1].w/100)*P[3].xyz*P[3].w);
    result[tid.xy]=writeColor(rawVignette(c,tid.xy),p.a);
}
float detailEdge(Texture2D<float4> plane,int2 p,int radius) {
    float center=plane.Load(int3(p,0)).r,sum=0;int count=0;
    for(int dy=-radius;dy<=radius;dy+=radius)for(int dx=-radius;dx<=radius;dx+=radius) {
        if(dx==0&&dy==0)continue;int2 q=p+int2(dx,dy);if(any(q<0)||any(q>=dimensions))continue;
        sum+=abs(plane.Load(int3(q,0)).r-center);++count;
    } return count?sum/count:0;
}
[numthreads(16,16,1)]
void rawChromaPlanes(uint3 tid:SV_DispatchThreadID){if(any(tid.xy>=dimensions))return;float4 p=source.Load(int3(tid.xy,0));float3 c=straight(p);float l=luminance709(c);result[tid.xy]=float4((c.r-l)*p.a,(c.b-l)*p.a,p.a,0);}
[numthreads(16,16,1)]
void rawChromaNormalize(uint3 tid:SV_DispatchThreadID){if(any(tid.xy>=dimensions))return;float4 p=source.Load(int3(tid.xy,0));result[tid.xy]=p.b>1e-6?float4(p.rg/p.b,0,0):float4(p.rg,0,0);}
[numthreads(16,16,1)]
void rawColorNoise(uint3 tid:SV_DispatchThreadID) {
    if(any(tid.xy>=dimensions))return;int2 at=tid.xy;int reach=(int)P[1].x;
    float2 dx=abs(auxiliary.Load(int3(clamp(at+int2(reach,0),0,dimensions-1),0)).rg-auxiliary.Load(int3(clamp(at-int2(reach,0),0,dimensions-1),0)).rg);
    float2 dy=abs(auxiliary.Load(int3(clamp(at+int2(0,reach),0,dimensions-1),0)).rg-auxiliary.Load(int3(clamp(at-int2(0,reach),0,dimensions-1),0)).rg);
    float local=P[1].y*(1-P[1].z*min(1,(dx.x+dx.y+dy.x+dy.y)*3));
    float4 p=source.Load(int3(at,0));float3 c=straight(p);float l=luminance709(c);float2 diff=lerp(c.rb-l,auxiliary.Load(int3(at,0)).rg,local);
    c.rb=l+diff;c.g=(l-.2126*c.r-.0722*c.b)/.7152;result[at]=writeColor(c,p.a);
}
[numthreads(16,16,1)]
void rawSharpen(uint3 tid:SV_DispatchThreadID) {
    if(any(tid.xy>=dimensions))return;float4 p=source.Load(int3(tid.xy,0));float edge=detailEdge(auxiliary,tid.xy,(int)P[1].x),threshold=P[1].w*.35;
    float mask=saturate((edge*(.5+P[1].z)-threshold)/max(.04,.35-threshold*.5));
    if(P[2].x>0){result[tid.xy]=writeColor(mask.xxx,p.a);return;}
    if(threshold==0)mask=1;float l=auxiliary.Load(int3(tid.xy,0)).r,high=l-third.Load(int3(tid.xy,0)).r;
    result[tid.xy]=writeColor(relight(straight(p),l+high*P[1].y*mask*1.9*(.5+P[1].z)),p.a);
}
[numthreads(16,16,1)]
void rawChromatic(uint3 tid:SV_DispatchThreadID) {
    if(any(tid.xy>=dimensions))return;float2 center=float2(dimensions)*.5;float radius=length(float2(tid.xy)+.5-center)/length(center),shift=.45*radius*radius*2.5;
    float4 p=source.Load(int3(tid.xy,0));float3 c=straight(p);
    c.r=straight(loadClamped(int2(floor((float)tid.x-shift+.5),tid.y))).r;
    c.b=straight(loadClamped(int2(floor((float)tid.x+shift+.5),tid.y))).b;result[tid.xy]=writeColor(c,p.a);
}
bool hueRange(float hue,float low,float high){return low<=high?(hue>=low&&hue<=high):(hue>=low||hue<=high);}
[numthreads(16,16,1)]
void rawOptics(uint3 tid:SV_DispatchThreadID) {
    if(any(tid.xy>=dimensions))return;float4 p=source.Load(int3(tid.xy,0));float3 c=straight(p);float3 hsl=rgbHsl(c);float hue=hsl.x*360;
    float reduce=0,hi=max(c.r,max(c.g,c.b)),lo=min(c.r,min(c.g,c.b)),chroma=hi-lo;
    bool purple=hueRange(hue,P[1].y,P[1].z),green=hueRange(hue,P[2].y,P[2].z);
#ifdef PRECISE_PREDICATES
    double3 native=double3(floor(p.rgb*maximum+.5))/max(1.0,double(floor(p.a*maximum+.5)));
    double mx=max(native.r,max(native.g,native.b)),mn=min(native.r,min(native.g,native.b)),dd=mx-mn;
    double h=dd<1e-12?0:mx==native.r?60*((native.g-native.b)/dd):mx==native.g?60*(2+(native.b-native.r)/dd):60*(4+(native.r-native.g)/dd);
    if(h<0)h+=360;
    purple=P[1].y<=P[1].z?(h>=P[1].y&&h<=P[1].z):(h>=P[1].y||h<=P[1].z);
    green=P[2].y<=P[2].z?(h>=P[2].y&&h<=P[2].z):(h>=P[2].y||h<=P[2].z);
#endif
    if(P[1].x>0&&purple)reduce=max(reduce,P[1].x/100);
    if(P[2].x>0&&green)reduce=max(reduce,P[2].x/100);
    if(chroma>1e-6&&reduce>0){float lum=luminance709(c);c=saturate(lum+(c-lum)*(1-reduce*chroma/hi));}
    if(P[3].x!=0) {
        float2 center=float2(dimensions)*.5;float distance=length(float2(tid.xy)+.5-center)/length(center)*(1+(50-P[3].y)/100);
        float stops=sign(P[3].x)*pow(abs(P[3].x)/100,1.15)*1.98*pow(distance,4.3),position=clamp((stops+5)/.5,0,20);
        int index=min(19,(int)position);c=saturate(lerp(sampleCube(auxiliary,c,17,index*17),sampleCube(auxiliary,c,17,(index+1)*17),position-index));
    }
    result[tid.xy]=writeColor(c,p.a);
}
[numthreads(16,16,1)]
void rawClip(uint3 tid:SV_DispatchThreadID) {
    if(any(tid.xy>=dimensions))return;float4 p=source.Load(int3(tid.xy,0));float3 c=straight(p);
    if(P[1].x>0&&any(c<=.5/maximum)){c*=.35;c.b+=.65;}
    if(P[1].y>0&&any(c>=(maximum-.5)/maximum)){c*=.35;c.r+=.65;}
    result[tid.xy]=writeColor(c,p.a);
}
)hlsl";
}
