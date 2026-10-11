#pragma once
namespace comp {
inline constexpr char gpuProcessingShader[] = R"hlsl(
Texture2D<float4> source : register(t0);
Texture2D<float4> auxiliary : register(t1);
Texture2D<float4> third : register(t2);
Texture2D<float4> fourth : register(t3);
RWTexture2D<float4> result : register(u0);
cbuffer Arguments : register(b0) { float4 P[64]; };
#define dimensions int2(P[0].xy)
#define maximum P[0].z
float4 quantizePixel(float4 p) { return floor(saturate(p)*maximum+.5)/maximum; }
float3 straight(float4 p) { return p.a>0?saturate(p.rgb/p.a):float3(0,0,0); }
float4 writeColor(float3 c,float a) { return quantizePixel(float4(saturate(c)*a,a)); }
float4 loadClamped(int2 p) { return source.Load(int3(clamp(p,int2(0,0),dimensions-1),0)); }
float4 samplePixel(float2 p,int2 size) {
    int2 b=(int2)floor(p);float2 f=frac(p);
    float4 a=source.Load(int3(b,0)),c=source.Load(int3(b+int2(1,0),0));
    float4 d=source.Load(int3(b+int2(0,1),0)),e=source.Load(int3(b+1,0));
    return lerp(lerp(a,c,f.x),lerp(d,e,f.x),f.y);
}
float luminance709(float3 c) { return dot(c,float3(.2126,.7152,.0722)); }
uint hash32(uint x) { x^=x>>16;x*=0x7feb352d;x^=x>>15;x*=0x846ca68b;x^=x>>16;return x; }
float noiseUnit(uint x) { return float(hash32(x)>>8)/16777216.; }
float lattice(int2 p,uint seed) {
    uint h=hash32((uint)p.x*0x9e3779b1 ^ hash32((uint)p.y*0x85ebca77 ^ seed));
    return float(h&65535)/65535.+float(h>>16)/65535.-1;
}
float grainField(float2 p,float size,uint seed) {
    float2 v=p/size;int2 i=(int2)floor(v);float2 t=frac(v);t=t*t*(3-2*t);
    return lerp(lerp(lattice(i,seed),lattice(i+int2(1,0),seed),t.x),
                lerp(lattice(i+int2(0,1),seed),lattice(i+1,seed),t.x),t.y)*1.6;
}
float3 rgbHsl(float3 c) {
    float hi=max(c.r,max(c.g,c.b)),lo=min(c.r,min(c.g,c.b)),d=hi-lo,l=(hi+lo)*.5;
    if(d<=0) return float3(0,0,l);
    float h=hi==c.r?(c.g-c.b)/d:hi==c.g?2+(c.b-c.r)/d:4+(c.r-c.g)/d;
    return float3(frac(h/6+1),d/(l<.5?hi+lo:2-hi-lo),l);
}
float hueChannel(float p,float q,float t) {
    t=frac(t+1);return t<1./6?p+(q-p)*6*t:t<.5?q:t<2./3?p+(q-p)*(2./3-t)*6:p;
}
float3 hslRgb(float3 hsl) {
    float q=hsl.z<.5?hsl.z*(1+hsl.y):hsl.z+hsl.y-hsl.z*hsl.y,p=2*hsl.z-q;
    return float3(hueChannel(p,q,hsl.x+1./3),hueChannel(p,q,hsl.x),hueChannel(p,q,hsl.x-1./3));
}
[numthreads(16,16,1)]
void nativeTone(uint3 tid:SV_DispatchThreadID){
    if(any(tid.xy>=dimensions))return;float4 p=source.Load(int3(tid.xy,0));uint4 native=(uint4)floor(p*maximum+.5);
    if(native.a==0){result[tid.xy]=p;return;}
    precise float3 unscaled=float3(native.rgb)*third.Load(int3(native.a&255,native.a>>8,0)).r;
    uint3 indices=(uint3)round(unscaled);float3 rgb=float3(auxiliary.Load(int3(indices.r&255,indices.r>>8,0)).r,auxiliary.Load(int3(indices.g&255,indices.g>>8,0)).g,auxiliary.Load(int3(indices.b&255,indices.b>>8,0)).b);
    result[tid.xy]=writeColor(rgb,p.a);
}
[numthreads(16,16,1)]
void pointAdjust(uint3 tid:SV_DispatchThreadID) {
    if(any(tid.xy>=dimensions)) return;
    int2 pos=tid.xy;float4 pixel=source.Load(int3(pos,0));float3 c=straight(pixel);
    int kind=(int)P[0].w;
    if(pixel.a==0) {result[pos]=pixel;return;}
    if(kind==1) { // Black & White: weighted primary and secondary components.
        float hi=max(c.r,max(c.g,c.b)),lo=min(c.r,min(c.g,c.b)),mid=c.r+c.g+c.b-hi-lo;
        int first=hi==c.r?0:hi==c.g?2:4;
        int second=hi==c.r?(c.g>=c.b?1:5):hi==c.g?(c.r>=c.b?1:3):(c.g>=c.r?3:5);
        float gray=saturate(lo+(mid-lo)*P[1+second/4][second%4]+(hi-mid)*P[1+first/4][first%4]);
        c=gray.xxx;
        if(P[3].z>0) c=hslRgb(float3(P[3].x/360,P[3].y,gray));
    } else if(kind==2) { // Per-channel tonal color balance.
        float original=dot(c,float3(.299,.587,.114));
        float3 shadows=saturate((c-.333)/-.25+.5)*.7;
        float3 highlights=saturate((c+.333-1)/.25+.5)*.7;
        float3 mids=saturate((c-.333)/.25+.5)*saturate((c+.333-1)/-.25+.5)*.7;
        c=saturate(c+P[1].xyz*shadows+P[2].xyz*mids+P[3].xyz*highlights);
        float now=dot(c,float3(.299,.587,.114));
        if(P[4].x>0&&now>.0001) c=saturate(c*original/now);
    } else if(kind==3) { // Native-depth gradient map, including integer luminance indexing.
        uint4 n=(uint4)floor(pixel*maximum+.5);uint m=(uint)maximum;
        uint3 rgb=min((n.rgb*m+n.a/2)/n.a,m);
        uint level=(dot(rgb,uint3(2126,7152,722))+5000)/10000;
        c=floor(saturate(lerp(P[1].xyz,P[2].xyz,min(level,m)/maximum))*maximum+.5)/maximum;
    } else if(kind==4) { // Seeded noise, with the same coordinate hash as the reference.
        uint seed=asuint(P[1].w);
        uint base=hash32(seed ^ hash32((uint)pos.x*0x9e3779b9 ^ hash32((uint)pos.y*0x85ebca6b)));
        for(int channel=0;channel<3;++channel) {
            uint key=P[1].z>0?base:base+(uint)channel*0x9e3779b9;
            float n=P[1].y>0?sqrt(-2*log(1-noiseUnit(key)))*cos(6.2831853*noiseUnit(key^0x68e31da4))*(2./3):noiseUnit(key)*2-1;
            c[channel]=saturate(c[channel]+n*P[1].x/200);
        }
    } else if(kind==5) {
        uint seed=asuint(P[1].w);float2 at=(float2(pos)+.5)*P[2].x;
        float broad=grainField(at,P[1].y,seed),fine=grainField(at,max(.5,P[1].y*.35),hash32(seed^0xa511e9b3));
        float l=luminance709(c),delta=lerp(broad,fine,saturate(P[1].z/100))*min(P[1].x/100,1)*.35*(.4+2.4*l*(1-l));
        c=saturate(c+delta);
    } else if(kind==6) {
        uint4 native=(uint4)floor(pixel*maximum+.5);
        precise float3 unscaled=float3(native.rgb)*auxiliary.Load(int3(native.a&255,native.a>>8,0)).r;
        c=round(unscaled)/maximum;
        // Qt QColor stores HSL at native 16-bit precision and hue in hundredths of degrees.
        float3 hsl=rgbHsl(c);hsl=floor(hsl*float3(36000,65535,65535)+.5)/float3(36000,65535,65535);
        float hue=hsl.x*360;float3 amount=0;
        for(int i=0;i<7;++i) {
            float4 adjustment=P[2+i*2],band=P[3+i*2];
            float weight=1;
            if(i>0) {
                float span=fmod(band.w-band.x+720,360),v=fmod(hue-band.x+720,360);
                float ramp=fmod(band.y-band.x+720,360),plateau=fmod(band.z-band.x+720,360);
                weight=span==0?1:v>span?0:v<ramp?v/max(.001,ramp):v<=plateau?1:(span-v)/max(.001,span-plateau);
                if(P[1].y>0&&P[1].z==i) weight=1-weight;
            }
            if(P[1].x>0) weight=P[1].z==i?1:0;
            amount+=adjustment.xyz*weight;
        }
        hsl.x=frac(((P[1].x>0?amount.x:hue+amount.x)+720)/360);
        float sat=clamp(amount.y/100,-1,1),light=clamp(amount.z/100,-1,1);
        hsl.y=P[1].x>0?saturate(amount.y/100):sat<=0?hsl.y*(1+sat):sat>=1?(hsl.y>0?1:0):min(1,hsl.y/(1-sat));
        hsl.z=light<0?hsl.z*(1+light):hsl.z+(1-hsl.z)*light;
        hsl=floor(hsl*float3(36000,65535,65535)+.5)/float3(36000,65535,65535);
        c=floor(saturate(hslRgb(hsl))*65535+.5)/65535;
        c=floor(c*maximum+.5)/maximum;
    }
    result[pos]=writeColor(c,pixel.a);
}
float cross2(float2 a,float2 b){return a.x*b.y-a.y*b.x;}
float2 transformedCorner(int i){int at=20+(i%4)*2;return float2(P[at/4][at%4],P[(at+1)/4][(at+1)%4]);}
float transformCoverage(float2 pos){
    bool full=true,inside=true;
    for(int edge=0;edge<4;++edge){float2 a=transformedCorner(edge),v=transformedCorner(edge+1)-a;float d=cross2(v,pos-a);
        if(d<0)inside=false;if(d<(abs(v.x)+abs(v.y))*.5)full=false;}
    if(P[4].z==0)return inside?1:0;
    if(full)return 1;
    float2 poly[12],temp[12];poly[0]=pos+float2(-.5,-.5);poly[1]=pos+float2(.5,-.5);poly[2]=pos+.5;poly[3]=pos+float2(-.5,.5);int count=4;
    for(int side=0;side<4;++side){float2 a=transformedCorner(side),v=transformedCorner(side+1)-a;int nextCount=0;
        for(int j=0;j<count;++j){float2 start=poly[j],finish=poly[(j+1)%count];float d0=cross2(v,start-a),d1=cross2(v,finish-a);
            if(d0>=0)temp[nextCount++]=start;if((d0>=0)!=(d1>=0))temp[nextCount++]=lerp(start,finish,d0/(d0-d1));}
        count=nextCount;for(int k=0;k<count;++k)poly[k]=temp[k];if(count==0)return 0;
    }
    float area=0;for(int n=1;n<count-1;++n)area+=cross2(poly[n]-poly[0],poly[n+1]-poly[0]);return saturate(area*.5);
}
[numthreads(16,16,1)]
void transformImage(uint3 tid:SV_DispatchThreadID) {
    if(any(tid.xy>=dimensions)) return;
    float3 pos=float3(float2(tid.xy)+.5,1);
    float z=dot(pos,P[3].xyz);
    float2 from=float2(dot(pos,P[1].xyz),dot(pos,P[2].xyz))/z-.5;
    float coverage=transformCoverage(float2(tid.xy)+.5);if(coverage==0){result[tid.xy]=0;return;}
    int2 b=(int2)floor(from),size=int2(P[4].xy);float2 f=frac(from);
    // Qt raster transforms clamp taps at image edges and use fixed-point interpolation weights.
    f=floor(f*256)/256;
    float4 a=source.Load(int3(clamp(b,0,size-1),0)),c=source.Load(int3(clamp(b+int2(1,0),0,size-1),0));
    float4 d=source.Load(int3(clamp(b+int2(0,1),0,size-1),0)),e=source.Load(int3(clamp(b+1,0,size-1),0));
    result[tid.xy]=quantizePixel(lerp(lerp(a,c,f.x),lerp(d,e,f.x),f.y)*coverage);
}
[numthreads(16,16,1)]
void motionBox(uint3 tid:SV_DispatchThreadID) {
    if(any(tid.xy>=dimensions)) return;
    int radius=(int)P[1].x;uint4 sum=0;
    for(int k=-radius;k<=radius;++k) sum+=(uint4)floor(source.Load(int3(int2(tid.xy)+int2(k,0),0))*maximum+.5);
    result[tid.xy]=float4((sum+radius)/(2*radius+1))/maximum;
}

float vignetteMask(float2 pos,float midpoint,float roundness,float feather) {
    float2 n=pos/float2(dimensions)*2-1;
    float circle=length(n)/sqrt(2.),square=max(abs(n.x),abs(n.y));
    float d=lerp(circle,square,(1-roundness/100)*.5);
    float t=saturate((d-midpoint*.0085)/max(.05,feather/100));return t*t*(3-2*t);
}
[numthreads(16,16,1)]
void standaloneFilter(uint3 tid:SV_DispatchThreadID) {
    if(any(tid.xy>=dimensions)) return;
    float4 pixel=source.Load(int3(tid.xy,0));float3 c=straight(pixel);int kind=(int)P[0].w;
    if(kind==1) {
        float mask=vignetteMask(float2(tid.xy)+.5,P[1].y,P[1].z,P[1].w);
        float bright=saturate((luminance709(c)-.45)/.55);
        c=lerp(c,P[2].xyz,saturate(P[1].x/100)*mask*(1-P[2].w/100*bright));
        pixel=writeColor(c,pixel.a);
    } else if(kind==2) {
        float3 light=auxiliary.Load(int3(tid.xy,0)).rgb;
        float3 added=light*P[1].x*pixel.a,room=(pixel.a-pixel.rgb)*.7;
        pixel.rgb+=float3(room.r>0?room.r*(1-exp(-added.r/room.r)):0,
                          room.g>0?room.g*(1-exp(-added.g/room.g)):0,room.b>0?room.b*(1-exp(-added.b/room.b)):0);
        pixel=quantizePixel(pixel);
    } else if(kind==3) {
        float4 base=auxiliary.Load(int3(tid.xy,0));
        if(pixel.a>0&&base.a>0) {
            float lum=luminance709(c),coarse=luminance709(straight(base));
            float sh=1-smoothstep(.15,.5,coarse),hi=smoothstep(.5,.85,coarse);
            float weight=(P[1].y*sh+P[1].z*(1-sh-hi)+P[1].w*hi)/100;
            float d=.18*tanh((lum-coarse)*6)*weight*(P[1].x/50)*(4*lum*(1-lum));
            pixel=writeColor(c+d,pixel.a);
        }
    } else if(kind==4) {
        float2 center=float2(dimensions)*.5,d=float2(tid.xy)+.5-center;
        float scale=1-P[1].x*dot(d,d)/dot(center,center);
        pixel=quantizePixel(samplePixel(center+d*scale-.5,dimensions));
    }
    result[tid.xy]=pixel;
}
)hlsl" R"hlsl(
[numthreads(16,16,1)]
void effectAlpha(uint3 tid:SV_DispatchThreadID) {
    if(any(tid.xy>=dimensions)) return;
    float a=source.Load(int3(tid.xy,0)).a;if(P[1].x>0)a=1-a;
    result[tid.xy]=a.xxxx;
}
[numthreads(16,16,1)]
void effectTint(uint3 tid:SV_DispatchThreadID) {
    if(any(tid.xy>=dimensions)) return;
    int2 from=int2(floor(float2(tid.xy)+.5-P[1].xy));
    float alpha=source.Load(int3(from,0)).a*P[2].w;
    result[tid.xy]=quantizePixel(float4(P[2].xyz*alpha,alpha));
}
[numthreads(16,16,1)]
void effectComposite(uint3 tid:SV_DispatchThreadID) {
    if(any(tid.xy>=dimensions))return;
    float4 back=source.Load(int3(tid.xy,0)),front=auxiliary.Load(int3(tid.xy,0));
    int mode=(int)P[1].x;
    if(mode==1) result[tid.xy]=quantizePixel(back*(1-front.a)+front);
    else if(mode==2) result[tid.xy]=quantizePixel(float4(back.rgb*(1-front.a)+front.rgb*back.a,back.a));
    else if(mode==3) result[tid.xy]=quantizePixel(back*front.a*P[1].y);
    else result[tid.xy]=0;
}
[numthreads(16,16,1)]
void effectOpacity(uint3 tid:SV_DispatchThreadID){
    if(any(tid.xy>=dimensions))return;float factor=P[1].x*(P[1].y>0?auxiliary.Load(int3(tid.xy,0)).a:1);
    result[tid.xy]=quantizePixel(source.Load(int3(tid.xy,0))*factor);
}
[numthreads(16,16,1)]
void effectColor(uint3 tid:SV_DispatchThreadID) {
    if(any(tid.xy>=dimensions))return;
    float4 p=source.Load(int3(tid.xy,0));
    result[tid.xy]=quantizePixel(float4(p.rgb*(1-P[1].w)+P[1].xyz*P[1].w*p.a,p.a));
}
[numthreads(16,16,1)]
void strokeRows(uint3 tid:SV_DispatchThreadID) {
    if(any(tid.xy>=dimensions))return;
    float left=1e10,right=1e10;int reach=(int)ceil(P[1].x)+1;bool inside=P[1].y>0;
    for(int dx=-reach;dx<=reach;++dx) {
        float a=source.Load(int3(int2(tid.xy)+int2(dx,0),0)).a;
        if((a>.5)!=inside){if(dx<=0)left=min(left,max(0,-float(dx)-.5));if(dx>=0)right=min(right,max(0,float(dx)-.5));}
    }
    result[tid.xy]=float4(min(left,right),left,right,0);
}
float2 strokeVertex(int i){int at=20+i*2;return float2(P[at/4][at%4],P[(at+1)/4][(at+1)%4]);}
float strokeHeight(float x){
    if(x<=0)return P[1].x;if(x>P[1].x)return -1e10;
    for(int i=0;i<(int)P[4].x;++i){float2 a=strokeVertex(i),b=strokeVertex(i+1);if(x<=b.x)return lerp(a.y,b.y,(x-a.x)/(b.x-a.x));}
    return 0;
}
[numthreads(16,16,1)]
void strokeColumns(uint3 tid:SV_DispatchThreadID) {
    if(any(tid.xy>=dimensions))return;bool inside=P[1].y>0;
    if(inside&&auxiliary.Load(int3(tid.xy,0)).a<=.5){result[tid.xy]=0;return;}
    int reach=(int)ceil(P[1].x)+1;float nearest=1e10;
    for(int dy=-reach;dy<=reach;++dy){int y=(int)tid.y+dy;float dx=y<0||y>=dimensions.y?(inside?0:1e10):source.Load(int3(tid.x,y,0)).r;
        nearest=min(nearest,length(float2(dx,max(0,abs(float(dy))-.5))));}
    float coverage=0;
    if(nearest<P[1].x-.707107)coverage=1;
    else if(nearest<P[1].x+.707107){
        // Integrate the union of offset row intervals. Multiple edges may cover different parts of one pixel.
        [loop]for(int sampleIndex=0;sampleIndex<64;++sampleIndex){float sx=(sampleIndex+.5)/64-.5,upper=-.5,lower=.5;bool filled=false;
            for(int dy=-reach;dy<=reach;++dy){int y=(int)tid.y+dy;float3 row=y<0||y>=dimensions.y?(inside?float3(0,0,0):float3(1e10,1e10,1e10)):source.Load(int3(tid.x,y,0)).rgb;
                float dx=row.r==0?0:min(row.g+sx,row.b-sx);float h=strokeHeight(dx);if(h<0)continue;
                if(dy==0){filled=true;break;}else if(dy<0)upper=max(upper,dy+.5+h);else lower=min(lower,dy-.5-h);
            }
            coverage+=filled?1:saturate(saturate(upper+.5)+saturate(.5-lower));
        }coverage/=64;
    }
    float a=coverage*P[2].w;result[tid.xy]=quantizePixel(float4(P[2].xyz*a,a));
}

groupshared uint4 prefixA[1024];
groupshared uint4 prefixB[1024];
[numthreads(256,1,1)]
void integerBox(uint3 group:SV_GroupID, uint index:SV_GroupIndex) {
    uint axis=(uint)P[1].x,radius=(uint)P[1].y,width=(uint)P[0].x,height=(uint)P[0].y;
    uint length=axis==0 ? width : height;
    uint start=group.x*256;
    for(uint i=index;i<1024;i+=256) {
        uint location=(uint)clamp((int)(start+i)-(int)radius,0,(int)length-1);
        uint2 pixel=axis==0 ? uint2(location,group.y) : uint2(group.y,location);
        prefixA[i]=i<256+2*radius ? (uint4)floor(saturate(source.Load(int3(pixel,0)))*maximum+.5) : uint4(0,0,0,0);
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
    result[target]=float4(sum/(2*radius+1))/maximum;
}

)hlsl";
}
