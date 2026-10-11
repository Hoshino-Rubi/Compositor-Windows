#pragma once
namespace comp {
inline constexpr char gpuAdjustmentBlend[] = R"hlsl(
float luminance(float3 c) { return dot(c, float3(.3,.59,.11)); }
float saturation(float3 c) { return max(c.r,max(c.g,c.b))-min(c.r,min(c.g,c.b)); }
float3 setLuminance(float3 c,float l) {
    c += l-luminance(c);
    float n=min(c.r,min(c.g,c.b)),x=max(c.r,max(c.g,c.b));
    if(n<0) c=l+(c-l)*l/(l-n);
    if(x>1) c=l+(c-l)*(1-l)/(x-l);
    return c;
}
float3 setSaturation(float3 c,float s) {
    float n=min(c.r,min(c.g,c.b)),x=max(c.r,max(c.g,c.b));
    return x>n ? (c-n)*s/(x-n) : float3(0,0,0);
}
float burn(float b,float s) { return b>=1?1:s<=0?0:1-min(1,(1-b)/s); }
float dodge(float b,float s) { return b<=0?0:s>=1?1:min(1,b/(1-s)); }
float blend(float b,float s) {
    switch(mode) {
    case 1:return min(b,s); case 2:return b*s; case 3:return burn(b,s);
    case 4:return max(0,b+s-1); case 5:return max(b,s); case 6:return b+s-b*s;
    case 7:return dodge(b,s); case 8:return min(1,b+s);
    case 9:return b<=.5?2*b*s:1-2*(1-b)*(1-s);
    case 10:return s<=.5?b-(1-2*s)*b*(1-b):b+(2*s-1)*((b<=.25?((16*b-12)*b+4)*b:sqrt(b))-b);
    case 11:return s<=.5?2*b*s:1-2*(1-b)*(1-s);
    case 12:return s<=.5?burn(b,2*s):dodge(b,2*s-1);
    case 13:return saturate(b+2*s-1); case 14:return s<=.5?min(b,2*s):max(b,2*s-1);
    case 15:return (s<=.5?burn(b,2*s):dodge(b,2*s-1))<.5?0:1;
    case 16:return abs(b-s); case 17:return b+s-2*b*s;
    case 18:return max(0,b-s); case 19:return s==0?1:min(1,b/s);
    default:return s;
    }
}
float3 unpremultiplyBlend(float4 c) {
    if(c.a<=0) return 0;
    return float3(c.r>=c.a?1:c.r/c.a,c.g>=c.a?1:c.g/c.a,c.b>=c.a?1:c.b/c.a);
}
float4 blendAdjusted(float4 bp,float4 sp) {
    if(mode==0) return sp;
    if(sp.a==0) return bp;
    float3 b=unpremultiplyBlend(bp),s=unpremultiplyBlend(sp),v;
    if(mode==20) v=setLuminance(setSaturation(s,saturation(b)),luminance(b));
    else if(mode==21) v=setLuminance(setSaturation(b,saturation(s)),luminance(b));
    else if(mode==22) v=setLuminance(s,luminance(b));
    else if(mode==23) v=setLuminance(b,luminance(s));
    #ifdef EXACT_HARD_MIX
    else if(mode==15) {
        double4 bi=double4(floor(bp*maximum+.5))/double(maximum);
        double4 si=double4(floor(sp*maximum+.5))/double(maximum);
        double3 bd=bi.a>0?bi.rgb/bi.a:double3(0,0,0),sd=si.rgb/si.a;
        for(int i=0;i<3;++i) {
            double mixed;
            if(sd[i]<=.5) mixed=bd[i]>=1?1:sd[i]<=0?0:1-min(1.0,(1-bd[i])/(2*sd[i]));
            else mixed=bd[i]<=0?0:sd[i]>=1?1:min(1.0,bd[i]/(1-(2*sd[i]-1)));
            v[i]=mixed<.5?0:1;
        }
    }
    #endif
    else v=float3(blend(b.r,s.r),blend(b.g,s.g),blend(b.b,s.b));
    float a=sp.a+bp.a*(1-sp.a);
    float3 c=(1-sp.a)*bp.a*b+(1-bp.a)*sp.a*s+bp.a*sp.a*v;
    return floor(float4(clamp(c,0,a),a)*maximum+.5)/maximum;
}
)hlsl";
}
