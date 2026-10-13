#pragma once
namespace comp {
inline constexpr char gpuWarpShader[] = R"hlsl(
// One quantized, source-snapshot warp dab.  The caller ping-pongs surfaces between
// dabs, so source and result are never the same resource during this dispatch.
precise float4 warpSample(int2 base, float2 fraction) {
    // Clamp the continuous coordinate exactly as the CPU sample() does.  The
    // caller has already split displacement into integer and fractional parts;
    // avoid reconstructing a large absolute float and losing the fraction.
    precise int2 last = dimensions - 1;
    if (base.x < 0) { base.x = 0; fraction.x = 0; }
    else if (base.x >= last.x) { base.x = last.x; fraction.x = 0; }
    if (base.y < 0) { base.y = 0; fraction.y = 0; }
    else if (base.y >= last.y) { base.y = last.y; fraction.y = 0; }
    precise int2 next = min(base + int2(1, 1), last);
    precise float4 topLeft = source.Load(int3(base, 0));
    precise float4 topRight = source.Load(int3(int2(next.x, base.y), 0));
    precise float4 bottomLeft = source.Load(int3(int2(base.x, next.y), 0));
    precise float4 bottomRight = source.Load(int3(next, 0));
    // The reference path interpolates native integer channels.  Reconstruct those
    // integers before interpolation instead of multiplying a normalized result at
    // the end; this keeps the 16-bit half-way decisions stable.
    precise uint4 nativeTopLeft = (uint4)floor(saturate(topLeft) * maximum + .5);
    precise uint4 nativeTopRight = (uint4)floor(saturate(topRight) * maximum + .5);
    precise uint4 nativeBottomLeft = (uint4)floor(saturate(bottomLeft) * maximum + .5);
    precise uint4 nativeBottomRight = (uint4)floor(saturate(bottomRight) * maximum + .5);
    precise float4 top = float4(nativeTopLeft) * (1 - fraction.x) + float4(nativeTopRight) * fraction.x;
    precise float4 bottom = float4(nativeBottomLeft) * (1 - fraction.x) + float4(nativeBottomRight) * fraction.x;
    // qRound for every native channel, including alpha, before a smudge mix.
    return floor((top * (1 - fraction.y) + bottom * fraction.y) + .5) / maximum;
}

[numthreads(16, 16, 1)]
void warpDab(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= dimensions))
        return;
    const int2 position = int2(tid.xy);
    precise float4 original = source.Load(int3(position, 0));
    const int flags = (int)P[2].w;
    if ((flags & 2) != 0 && auxiliary.Load(int3(position, 0)).r <= .5) {
        result[position] = original;
        return;
    }
    precise float2 pixelCenter = float2(position) + .5;
    precise float u = length(pixelCenter - P[1].xy) / P[1].z;
    if (u >= 1 || P[1].z < 1) {
        result[position] = original;
        return;
    }
    precise float weight = u <= P[1].w ? 1 : (1 - u) / (1 - P[1].w);
    weight = weight * weight * (3 - 2 * weight);
    precise float displacementFactor = (flags & 1) != 0 ? 1 : weight;
    precise float2 displacement = -P[2].xy * displacementFactor;
    precise float2 wholeDisplacement = floor(displacement);
    precise float2 fraction = displacement - wholeDisplacement;
    precise int2 sampleBase = position + (int2)wholeDisplacement;
    precise float4 sampled = warpSample(sampleBase, fraction);
    if ((flags & 1) != 0) {
        precise float amount = weight * P[2].z;
        precise uint4 originalNative = (uint4)floor(saturate(original) * maximum + .5);
        precise uint4 sampledNative = (uint4)floor(saturate(sampled) * maximum + .5);
        sampled = floor(float4(originalNative) * (1 - amount) + float4(sampledNative) * amount + .5) / maximum;
    }
    result[position] = sampled;
}
)hlsl";
}
