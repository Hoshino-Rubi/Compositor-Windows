#pragma once

namespace comp {

// The layer-surface pass deliberately consumes the original image and mask
// textures.  No QImage containing a prepared/flattened layer is uploaded: the
// placement transform, native-depth rounding, opacity and mask coverage all
// happen in the same D3D pass.  The other entry points apply one additional
// clipping/ancestor coverage to an already prepared floating-point surface.
inline constexpr char gpuLayerSurfaceShader[] = R"hlsl(
Texture2D<float4> imageSource : register(t0);
Texture2D<float> maskSource : register(t1);
Texture2D<float4> auxiliary : register(t2);
RWTexture2D<float4> result : register(u0);

cbuffer Arguments : register(b0) {
    float4 P[64];
};

#define dimensions int2(P[0].xy)
#define maximum P[0].z
#define HAS_MASK 1
#define HAS_AUXILIARY 2

float4 quantizePixel(float4 p) {
    return floor(saturate(p) * maximum + .5) / maximum;
}

int cornerBase(int base) {
    return base == 1 ? 5 : 13;
}

float2 transformedCorner(int base, int i) {
    return P[cornerBase(base) + (i & 3)].xy;
}

bool excludesRasterBoundary(float2 a, float2 b, int base) {
    const int corner = cornerBase(base);
    float minX = P[corner].x, minY = P[corner].y;
    [unroll]
    for (int i = 1; i < 4; ++i) {
        minX = min(minX, P[corner + i].x);
        minY = min(minY, P[corner + i].y);
    }
    // QPainter's non-antialiased image path uses a half-open fill rule: the
    // top and left edges are excluded when a pixel center lands exactly on an
    // edge, while bottom/right edges remain covered.  This matters for a
    // half-pixel translation (e.g. y=8.5) even though all interior samples
    // still use nearest-neighbor filtering.
    const float epsilon = 1e-6;
    const bool horizontalTop = abs(a.y - b.y) <= epsilon && abs(a.y - minY) <= epsilon;
    const bool verticalLeft = abs(a.x - b.x) <= epsilon && abs(a.x - minX) <= epsilon;
    return horizontalTop || verticalLeft;
}

float2 inverseMap(float2 position, int base) {
    float3 p = float3(position, 1);
    float z = dot(p, P[base + 2].xyz);
    return float2(dot(p, P[base].xyz), dot(p, P[base + 1].xyz)) / z - .5;
}

// QPainter's source rectangle can be reflected by a flipped placement.  The
// older generic transform shader assumed a counter-clockwise polygon, which
// incorrectly rejected every sample of a reflected layer.  Normalize the
// edge sign before clipping so the source and mask paths agree with QPainter.
float polygonSign(int base) {
    float area = 0;
    for (int i = 0; i < 4; ++i) {
        float2 a = transformedCorner(base, i);
        float2 b = transformedCorner(base, i + 1);
        area += a.x * b.y - a.y * b.x;
    }
    return area < 0 ? -1 : 1;
}

float polygonCoverage(float2 position, int base) {
    bool full = true, inside = true;
    float sign = polygonSign(base);
    for (int edge = 0; edge < 4; ++edge) {
        float2 a = transformedCorner(base, edge);
        float2 v = transformedCorner(base, edge + 1) - a;
        float d = sign * (v.x * (position.y - a.y) - v.y * (position.x - a.x));
        if (d < 0 || (abs(d) <= 1e-6 && excludesRasterBoundary(a, a + v, base))) inside = false;
        if (d < (abs(v.x) + abs(v.y)) * .5) full = false;
    }
    if ((base == 1 ? P[4].z : P[12].z) == 0) return inside ? 1 : 0;
    if (full) return 1;

    float2 poly[12], temp[12];
    poly[0] = position + float2(-.5, -.5);
    poly[1] = position + float2(.5, -.5);
    poly[2] = position + float2(.5, .5);
    poly[3] = position + float2(-.5, .5);
    int count = 4;
    for (int side = 0; side < 4; ++side) {
        float2 a = transformedCorner(base, side);
        float2 v = transformedCorner(base, side + 1) - a;
        int nextCount = 0;
        for (int j = 0; j < count; ++j) {
            float2 start = poly[j], finish = poly[(j + 1) % count];
            float d0 = sign * (v.x * (start.y - a.y) - v.y * (start.x - a.x));
            float d1 = sign * (v.x * (finish.y - a.y) - v.y * (finish.x - a.x));
            if (d0 >= 0) temp[nextCount++] = start;
            if ((d0 >= 0) != (d1 >= 0)) temp[nextCount++] = lerp(start, finish, d0 / (d0 - d1));
        }
        count = nextCount;
        for (int k = 0; k < count; ++k) poly[k] = temp[k];
        if (count == 0) return 0;
    }
    float area = 0;
    for (int n = 1; n < count - 1; ++n) {
        float2 a = poly[n] - poly[0], b = poly[n + 1] - poly[0];
        area += a.x * b.y - a.y * b.x;
    }
    return saturate(abs(area) * .5);
}

float4 sampleImage(float2 position) {
    int2 size = int2(P[4].xy);
    float2 from = inverseMap(position, 1);
    if (P[4].w == 1) {
        // TxTranslate is routed by Qt through its untransformed fetcher.  Qt
        // rounds the painter translation to an integer before indexing the
        // source scanline.
        int2 nearest = int2(floor(position) - P[19].zw);
        return imageSource.Load(int3(clamp(nearest, 0, size - 1), 0));
    }
    if (P[4].w == 2) {
        // The generic transformed fetcher samples the raw inverse coordinate
        // at the output pixel center.
        int2 nearest;
        if (maximum <= 255.5) {
            int2 pixel = int2(floor(position));
            int u = pixel.x * int(P[22].x) + pixel.y * int(P[22].y) + int(P[22].z);
            int v = pixel.x * int(P[23].x) + pixel.y * int(P[23].y) + int(P[23].z);
            nearest = int2(u >> 16, v >> 16);
        } else nearest = int2(floor(from + .5));
        return imageSource.Load(int3(clamp(nearest, 0, size - 1), 0));
    }
    if (P[4].w == 3) {
        // QPainter's non-antialiased TxScale path snaps the destination
        // rectangle before fetching.  Its source index is relative to the
        // snapped integer rectangle, not the continuous inverse matrix.
        float2 origin = P[18].xy;
        float2 extent = max(P[18].zw, 1);
        float2 local = (position - origin) * float2(size) / extent;
        if (P[19].x < 0) local.x = float(size.x) - local.x;
        if (P[19].y < 0) local.y = float(size.y) - local.y;
        int2 nearest = int2(floor(local));
        return imageSource.Load(int3(clamp(nearest, 0, size - 1), 0));
    }
    int2 b = int2(floor(from));
    float2 f = frac(from);
    // SmoothPixmapTransform uses the native source samples and a floating
    // bilinear weight.  Do not quantize the fraction to the processing
    // shader's historical 1/256 approximation: at 16-bit depth that loses
    // several native levels on rotated/scaled surfaces.
    float4 a = imageSource.Load(int3(clamp(b, 0, size - 1), 0));
    float4 c = imageSource.Load(int3(clamp(b + int2(1, 0), 0, size - 1), 0));
    float4 d = imageSource.Load(int3(clamp(b + int2(0, 1), 0, size - 1), 0));
    float4 e = imageSource.Load(int3(clamp(b + 1, 0, size - 1), 0));
    return lerp(lerp(a, c, f.x), lerp(d, e, f.x), f.y);
}

float sampleMask(float2 position) {
    int2 size = int2(P[12].xy);
    float2 from = inverseMap(position, 9);
    if (P[12].w == 1) {
        int2 nearest = int2(floor(position) - P[21].zw);
        return maskSource.Load(int3(clamp(nearest, 0, size - 1), 0));
    }
    int2 b = int2(floor(from));
    float2 f = frac(from);
    // Masks follow QPainter's SmoothPixmapTransform sampling as well; retain
    // full float precision and quantize only the resulting coverage below.
    float a = maskSource.Load(int3(clamp(b, 0, size - 1), 0));
    float c = maskSource.Load(int3(clamp(b + int2(1, 0), 0, size - 1), 0));
    float d = maskSource.Load(int3(clamp(b + int2(0, 1), 0, size - 1), 0));
    float e = maskSource.Load(int3(clamp(b + 1, 0, size - 1), 0));
    return lerp(lerp(a, c, f.x), lerp(d, e, f.x), f.y);
}

[numthreads(16, 16, 1)]
void layer(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= dimensions)) return;
    float2 position = float2(tid.xy) + .5;
    float coverage;
    if (maximum == 255 && P[4].w == 2) {
        // Qt's 8-bit affine nearest path clips from the inverse source
        // coordinate.  At a rotated right edge the polygon corners are
        // quantized independently and can reject a source sample that is
        // still strictly inside the half-open image bounds.
        float2 source = inverseMap(position, 1) + .5;
        coverage = all(source >= 0 && source < P[4].xy) ? 1 : 0;
    } else {
        coverage = polygonCoverage(position, 1);
    }
    if (coverage == 0) {
        result[tid.xy] = 0;
        return;
    }
    float4 pixel = quantizePixel(sampleImage(position) * coverage);
    // Opacity is rounded to the document's native integer depth before the
    // destination-in mask multiply, matching coverage()/QPainter's raster
    // quantization at a layer boundary.
    pixel = quantizePixel(pixel * P[17].x);
    if (((uint)P[0].w & HAS_MASK) != 0) {
        float mask = sampleMask(position) * polygonCoverage(position, 9);
        mask = floor(saturate(mask) * maximum + .5) / maximum;
        pixel = quantizePixel(pixel * mask);
    }
    result[tid.xy] = pixel;
}

[numthreads(16, 16, 1)]
void multiplyMask(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= dimensions)) return;
    float2 position = float2(tid.xy) + .5;
    float mask = sampleMask(position) * polygonCoverage(position, 9);
    mask = floor(saturate(mask) * maximum + .5) / maximum;
    result[tid.xy] = quantizePixel(imageSource.Load(int3(tid.xy, 0)) * mask * P[17].x);
}

[numthreads(16, 16, 1)]
void multiplySurface(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= dimensions)) return;
    float factor = P[17].x;
    if (((uint)P[0].w & HAS_AUXILIARY) != 0)
        factor *= auxiliary.Load(int3(tid.xy, 0)).a;
    result[tid.xy] = quantizePixel(imageSource.Load(int3(tid.xy, 0)) * factor);
}
)hlsl";

} // namespace comp
