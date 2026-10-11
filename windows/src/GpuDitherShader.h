#pragma once
namespace comp {
inline constexpr char gpuDitherShader[] = R"hlsl(
float3 toneValue(float3 rgb,float gamma,float contrast){return saturate((pow(saturate(rgb),gamma)-.5)*contrast+.5);}
uint4 resizeRow(int2 first,uint weight,uint fullWeight,int2 size) {
    uint4 sum=(uint4)floor(source.Load(int3(clamp(first,0,size-1),0))*maximum+.5)*weight;
    uint left=16384-weight;int x=first.x;
    while(left>0){++x;uint part=min(left,fullWeight);sum+=(uint4)floor(source.Load(int3(min(x,size.x-1),first.y,0))*maximum+.5)*part;left-=part;}
    return sum;
}
[numthreads(16,16,1)]
void ditherResize(uint3 tid:SV_DispatchThreadID){
    if(any(tid.xy>=dimensions))return;uint2 size=(uint2)P[1].xy;
    if(all(size==(uint2)dimensions)){result[tid.xy]=floor(source.Load(int3(tid.xy,0))*255+.5)/255;return;}
    uint2 stepSize=(size*65536)/(uint2)dimensions,pos=tid.xy*stepSize,first=pos>>16;
    uint2 fullWeight=((uint2)dimensions*16384+size-1)/size,weight=((65536-(pos&65535))*fullWeight)>>16;
    uint4 low=0,high=0;uint remaining=16384;int y=first.y;uint part=weight.y;
    while(remaining>0){
        uint4 row=resizeRow(int2(first.x,min(y,(int)size.y-1)),weight.x,fullWeight.x,size);
        if(maximum==255)low+=(row>>4)*part;
        else {for(int c=0;c<4;++c){uint productLow=(row[c]&65535)*part,productHigh=(row[c]>>16)*part;uint lo=productLow+(productHigh<<16),hi=(productHigh>>16)+(lo<productLow?1:0);uint old=low[c];low[c]+=lo;high[c]+=hi+(low[c]<old?1:0);}}
        remaining-=part;part=min(remaining,fullWeight.y);++y;
    }
    uint4 value=maximum==255?low>>24:(low>>28)|(high<<4);
    if(maximum>255)value=(value+128)/257;
    result[tid.xy]=float4(value)/255;
}
[numthreads(16,16,1)]
void ditherTone(uint3 tid:SV_DispatchThreadID){
    if(any(tid.xy>=dimensions))return;float4 pixel=source.Load(int3(tid.xy,0));float4 native=floor(pixel*255+.5);precise float3 rgb=native.a>0?native.rgb*(1/native.a):float3(0,0,0);
    if(P[2].x==0)rgb=luminance709(rgb).xxx;
    result[tid.xy]=float4(toneValue(rgb,P[1].z,P[1].w),pixel.a);
}
static const uint ditherBayer[64]={0,32,8,40,2,34,10,42,48,16,56,24,50,18,58,26,12,44,4,36,14,46,6,38,60,28,52,20,62,30,54,22,3,35,11,43,1,33,9,41,51,19,59,27,49,17,57,25,15,47,7,39,13,45,5,37,63,31,55,23,61,29,53,21};
static const uint ditherPatterns[136]={
0,0,0,0,0,0,0,0, 128,0,0,0,8,0,0,0, 136,0,34,0,136,0,34,0,128,64,32,16,8,4,2,1,
136,34,136,34,136,34,136,34, 0,255,0,0,0,255,0,0,17,34,68,136,17,34,68,136,170,0,170,0,170,0,170,0,
136,85,34,85,136,85,34,85,255,128,128,128,255,8,8,8,170,85,170,85,170,85,170,85,129,66,36,24,24,36,66,129,
119,170,221,170,119,170,221,170,238,221,187,119,238,221,187,119,119,255,221,255,119,255,221,255,127,255,255,255,247,255,255,255,255,255,255,255,255,255,255,255};
[numthreads(16,16,1)]
void ditherGlyphSelect(uint3 tid:SV_DispatchThreadID){
    if(any(tid.xy>=dimensions))return;int2 cell=int2(P[3].zw);int2 start=int2(tid.xy)*cell;float sum=0;int count=0;
    for(int y=start.y;y<min(start.y+cell.y,(int)P[6].z);++y)for(int x=start.x;x<min(start.x+cell.x,(int)P[6].y);++x){
        float4 pixel=source.Load(int3(x,y,0));if(pixel.a>0){sum+=luminance709(pixel.rgb);++count;}}
    float average=count?sum/count:1,wanted=(P[2].y>0?average:1-average)*P[32+((int)P[4].w-1)/4][((int)P[4].w-1)%4];
    int best=0;float distance=2;for(int g=0;g<(int)P[4].w;++g){float d=abs(P[32+g/4][g%4]-wanted);if(d<distance){best=g;distance=d;}}
    result[tid.xy]=float4(best,0,0,0);
}
[numthreads(16,16,1)]
void ditherMarks(uint3 tid:SV_DispatchThreadID){
    if(any(tid.xy>=dimensions))return;float4 pixel=source.Load(int3(tid.xy,0));if(pixel.a==0){result[tid.xy]=0;return;}
    float3 value=auxiliary.Load(int3(tid.xy,0)).rgb,c;int style=(int)P[1].x;float steps=P[1].y-1;
    if(style<=4){
        if(style>=2){int side=style==2?2:style==3?4:8;int index=(tid.y%side)*8+tid.x%side;float threshold=(ditherBayer[index]/(64/side/side)+.5)/(side*side);value=min(steps,floor(saturate(value)*steps+threshold))/steps;}
        c=P[2].x>0?value:lerp(P[4].xyz,P[5].xyz,value.x);
    }else{
        float t=luminance709(value),coverage=P[2].y>0?t:1-t,amount=0;
        if(style==9){int2 cell=int2(P[3].zw);int glyph=(int)third.Load(int3(int2(tid.xy)/cell,0)).r;amount=fourth.Load(int3(tid.x%cell.x,glyph*cell.y+tid.y%cell.y,0)).r;}
        else if(style==8){int index=(int)floor(coverage*16+.5);amount=(ditherPatterns[index*8+(tid.y&7)]>>(7-(tid.x&7)))&1;}
        else{float2 xy=float2(tid.xy)+.5;float2 uv=frac(float2(xy.x*P[3].x+xy.y*P[3].y,-xy.x*P[3].y+xy.y*P[3].x)/P[2].w)-.5;
            float spot=style==5?3.14159265*dot(uv,uv):style==6?abs(uv.y)*2:abs(uv.x)+abs(uv.y);amount=coverage>spot?1:0;}
        if(P[2].x>0)c=lerp((P[2].y>0?0:1).xxx,straight(pixel),amount);
        else c=lerp(P[2].y>0?P[4].xyz:P[5].xyz,P[2].y>0?P[5].xyz:P[4].xyz,amount);
    }
    result[tid.xy]=floor(float4(saturate(c)*pixel.a,pixel.a)*255+.5)/255;
}
[numthreads(16,16,1)]
void ditherFinish(uint3 tid:SV_DispatchThreadID){
    if(any(tid.xy>=dimensions))return;int2 size=int2(P[6].yz);uint2 increments=((uint2)size*65536)/(uint2)dimensions;int2 pos=min(size-1,int2(((tid.xy*2+1)*increments)>>17));
    float4 p=source.Load(int3(pos,0));int block=(int)P[5].w;
    if(P[6].x>0&&block>1){float2 d=float2(tid.xy%block)+.5-block*.5;float coverage=saturate(block*.42-length(d)+.5);p.rgb=floor((p.rgb*coverage+P[4].xyz*p.a*(1-coverage))*255+.5)/255;}
    if(maximum>255){uint oldAlpha=(uint)floor(p.a*255+.5);uint newAlpha=(uint)floor(auxiliary.Load(int3(tid.xy,0)).a*maximum+.5);uint3 rgb=(uint3)floor(p.rgb*255+.5);p=float4(oldAlpha?(rgb*newAlpha)/oldAlpha:uint3(0,0,0),newAlpha)/maximum;}
    result[tid.xy]=p;
}
[numthreads(16,16,1)]
void scanSample(uint3 tid:SV_DispatchThreadID){
    if(any(tid.xy>=dimensions))return;int spacing=(int)P[1].x;float lineY=tid.y;
    float wave=sin(lineY*.45)*.7+sin(lineY*1.7+1.3)*.3,offset=P[1].w*wave;int shift=(int)(sign(offset)*floor(abs(offset)+.5));int x=(int)tid.x-shift;
    float3 sum=0;int count=0;
    if(x>=0&&x<dimensions.x)for(int y=tid.y*spacing;y<min((int)(tid.y+1)*spacing,(int)P[6].y);++y){
        float4 p=source.Load(int3(x,y,0));if(p.a==0)continue;float3 rgb=straight(p);if(P[3].w==0)rgb=luminance709(rgb).xxx;
        sum+=toneValue(rgb,P[3].x,P[3].y);++count;}
    float3 rgb=count?sum/count:float3(0,0,0);result[tid.xy]=float4(rgb,luminance709(rgb));
}
[numthreads(16,16,1)]
void scanSmooth(uint3 tid:SV_DispatchThreadID){
    if(any(tid.xy>=dimensions))return;
    if(P[7].y>0){result[tid.xy]=(loadClamped(int2(tid.xy)-int2(0,1))+2*loadClamped(tid.xy)+loadClamped(int2(tid.xy)+int2(0,1)))/4;return;}
    if(tid.x!=0)return;int radius=(int)P[7].x;precise float4 sum=0;
    for(int x=-radius;x<=radius;++x)sum+=loadClamped(int2(x,tid.y));
    for(int x=0;x<dimensions.x;++x){result[int2(x,tid.y)]=sum/(2*radius+1);sum+=loadClamped(int2(x+radius+1,tid.y))-loadClamped(int2(x-radius,tid.y));}
}
// Each invocation owns a column: order/occlusion are deterministic without inter-thread UAV races.
[numthreads(16,16,1)]
void scanDraw(uint3 tid:SV_DispatchThreadID){
    if(tid.x>=dimensions.x||tid.y!=0)return;int x=tid.x;
    for(int y=0;y<dimensions.y;++y)result[int2(x,y)]=0;
    float middle=P[1].x*.5,along=fmod(x+.5,P[1].x)-middle;bool rising=P[2].x>=0;float horizon=rising?1e20:-1e20;
    for(int stepIndex=0;stepIndex<(int)P[6].z;++stepIndex){
        int lineIndex=rising?(int)P[6].z-1-stepIndex:stepIndex;float tone=source.Load(int3(x,lineIndex,0)).a;
        float beading=P[1].z>0?saturate((P[1].z-tone)/.2):0,across=along*beading;int at=clamp((int)floor(x-across+.5),0,dimensions.x-1);
        float4 v=source.Load(int3(at,lineIndex,0));float lit=P[2].z>0?saturate((v.a-P[2].z)/.04):1;if(lit<=0)continue;
        float base=lineIndex*P[1].x+middle,here=base-P[2].x*auxiliary.Load(int3(at,lineIndex,0)).a;
        float before=at>0?base-P[2].x*auxiliary.Load(int3(at-1,lineIndex,0)).a:here,lo=min(here,before),hi=max(here,before);
        float level=P[3].z+(1-P[3].z)*v.a;float3 c=P[3].w>0?P[3].z+(1-P[3].z)*v.rgb:lerp(P[4].xyz,P[5].xyz,level);
        float beam=middle*P[1].y*(.29+.71*sqrt(saturate(level)));
        int top=max(0,(int)floor(rising?lo-beam-1:max(lo-beam-1,horizon-1)));
        int bottom=min(dimensions.y,(int)ceil(rising?min(hi+beam+1,horizon+1):hi+beam+1));
        for(int y=top;y<bottom;++y){float yy=y+.5,off=max(0,max(lo-yy,yy-hi));float shown=saturate(beam-length(float2(off,across))+.5)*lit*saturate(rising?horizon-yy+.5:yy-horizon+.5);
            if(shown>result[int2(x,y)].a)result[int2(x,y)]=float4(c,shown);}
        if(lit>.5)horizon=rising?min(horizon,lo-beam):max(horizon,hi+beam);
    }
}
[numthreads(16,16,1)]
void scanFinish(uint3 tid:SV_DispatchThreadID){
    if(any(tid.xy>=dimensions))return;float4 cover=source.Load(int3(tid.xy,0));float a=auxiliary.Load(int3(tid.xy,0)).a;
    result[tid.xy]=writeColor(P[4].xyz+(cover.rgb*1.35-P[4].xyz)*cover.a,a);
}
[numthreads(16,16,1)]
void scanSplit(uint3 tid:SV_DispatchThreadID){
    if(any(tid.xy>=dimensions))return;float4 p=source.Load(int3(tid.xy,0));int split=(int)P[2].w;
    p.r=min(p.a,source.Load(int3(int2(tid.xy)-int2(split,0),0)).r);p.b=min(p.a,source.Load(int3(int2(tid.xy)+int2(split,0),0)).b);result[tid.xy]=p;
}
)hlsl";
}
