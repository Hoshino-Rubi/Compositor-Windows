#pragma once

// Standalone Direct3D 11 compute shader for a perspective image transform.
// The source and destination textures use premultiplied RGBA values. Sampling
// follows Qt's native projective path: 8-bit uses its 1/256 fixed-point
// interpolator, while 16-bit retains the higher-precision native blend.
namespace comp {
inline constexpr char gpuPerspectiveShader[] = R"hlsl(
Texture2D<float4> perspectiveSource : register(t0);
Texture2D<int2> perspectiveSpans : register(t1);
RWTexture2D<float4> perspectiveResult : register(u0);

cbuffer PerspectiveArguments : register(b0) {
    // P[0] output width/height and native channel maximum.
    // P[1..3] are the three rows used by QTransform's inverse mapping.
    // P[4] input width/height, antialias flag and transparent-outside flag.
    // P[5..7] are reserved for the CPU geometry path.
    float4 P[8];
};

int2 perspectiveDimensions() { return int2(P[0].xy); }
int2 perspectiveInputSize() { return int2(P[4].xy); }
float perspectiveMaximum() { return P[0].z; }

// The CPU rasterizer supplies Qt's exact half-open span for every output row.
// Keeping coverage out of the pixel kernel avoids reimplementing Qt's fixed
// point edge stepping with shader floats.
float perspectiveCoverage(int2 pixel) {
    const int2 span = perspectiveSpans.Load(int3(0, pixel.y, 0));
    return pixel.x >= span.x && pixel.x < span.y ? 1 : 0;
}

float4 perspectiveQuantize(float4 value) {
    return floor(saturate(value) * perspectiveMaximum() + .5) / perspectiveMaximum();
}

float4 perspectiveLoadClamped(int2 position) {
    int2 last = perspectiveInputSize() - 1;
    return perspectiveSource.Load(int3(clamp(position, int2(0, 0), last), 0));
}

float4 perspectiveSample(float2 sourceCoordinate) {
    int2 base = int2(floor(sourceCoordinate));
    // Keep the source fraction at shader precision. The 8-bit branch below
    // derives Qt's fixed-point distance; the 16-bit branch uses the full
    // fraction after reconstructing native taps.
    float2 fraction = frac(sourceCoordinate);
    float4 topLeft = perspectiveLoadClamped(base);
    float4 topRight = perspectiveLoadClamped(base + int2(1, 0));
    float4 bottomLeft = perspectiveLoadClamped(base + int2(0, 1));
    float4 bottomRight = perspectiveLoadClamped(base + int2(1, 1));
    // Qt's raster path interpolates native integer channels, then rounds the
    // result before writing the destination.  Reconstructing the native taps
    // here is important for 16-bit documents: interpolating normalized UNORM
    // values directly can lose a native step at fixed-point boundaries.
    const float maximum = perspectiveMaximum();
    const uint4 nativeTopLeft = (uint4)floor(saturate(topLeft) * maximum + .5);
    const uint4 nativeTopRight = (uint4)floor(saturate(topRight) * maximum + .5);
    const uint4 nativeBottomLeft = (uint4)floor(saturate(bottomLeft) * maximum + .5);
    const uint4 nativeBottomRight = (uint4)floor(saturate(bottomRight) * maximum + .5);
    if (maximum <= 255.5) {
        // Qt's projective ARGB32 path uses the generic four-pixel
        // interpolator: truncate the vertical blend to native bytes first,
        // then truncate the horizontal blend.  Keeping both truncations and
        // the 8-bit fixed-point distances is observable on smooth gradients.
        const uint2 fixedDistance = (uint2)floor(saturate(fraction) * 256);
        const uint4 left8 =
            (nativeTopLeft * (256 - fixedDistance.y) + nativeBottomLeft * fixedDistance.y) / 256;
        const uint4 right8 =
            (nativeTopRight * (256 - fixedDistance.y) + nativeBottomRight * fixedDistance.y) / 256;
        const uint4 value8 = (left8 * (256 - fixedDistance.x) + right8 * fixedDistance.x) / 256;
        return float4(value8) / maximum;
    }
    const float4 top = float4(nativeTopLeft) * (1 - fraction.x) + float4(nativeTopRight) * fraction.x;
    const float4 bottom = float4(nativeBottomLeft) * (1 - fraction.x) + float4(nativeBottomRight) * fraction.x;
    return floor((top * (1 - fraction.y) + bottom * fraction.y) + .5) / maximum;
}

[numthreads(16, 16, 1)]
void perspectiveWarp(uint3 threadId : SV_DispatchThreadID) {
    const int2 outputSize = perspectiveDimensions();
    if (any(int2(threadId.xy) >= outputSize))
        return;
    const float2 outputPosition = float2(threadId.xy) + .5;
    const float3 homogeneous = float3(outputPosition, 1);
    const float denominator = dot(homogeneous, P[3].xyz);
    if (abs(denominator) < 1e-8) {
        perspectiveResult[threadId.xy] = 0;
        return;
    }
    const float2 sourcePosition =
        float2(dot(homogeneous, P[1].xyz), dot(homogeneous, P[2].xyz)) / denominator - .5;
    const float coverage = P[4].w != 0 ? perspectiveCoverage(int2(threadId.xy)) : 1;
    if (coverage <= 0) {
        perspectiveResult[threadId.xy] = 0;
        return;
    }
    perspectiveResult[threadId.xy] = perspectiveQuantize(perspectiveSample(sourcePosition) * coverage);
}
)hlsl";
}
