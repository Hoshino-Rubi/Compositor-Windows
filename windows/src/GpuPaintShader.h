#pragma once

namespace comp {

// Integer textures are used deliberately.  The CPU painter rounds at every
// dab on an 8/16-bit premultiplied image; quantizing after each dab here keeps
// subsequent dabs on the same native lattice instead of accumulating float
// error until the final readback.
inline constexpr char gpuPaintShader[] = R"hlsl(
Texture2D<uint4> targetImage : register(t0);
Texture2D<uint4> sourceImage : register(t1);
Texture2D<float> coverageImage : register(t2);
Texture2D<float> selectionImage : register(t3);

struct DabInfo {
    int4 target;       // x, y, width, height in the cropped target
    int4 atlas;        // x, y; remaining fields reserved
    float4 source;     // sourceOffset.x, sourceOffset.y, flow, reserved
};
StructuredBuffer<DabInfo> dabs : register(t4);
RWTexture2D<uint4> outputImage : register(u0);

cbuffer Settings : register(b0) {
    uint width;
    uint height;
    uint dabCount;
    uint operation;    // 0 brush, 1 eraser, 2 clone, 3 blur
    float maximum;
    uint sourceWidth;
    uint sourceHeight;
    uint hasSource;
    uint hasSelection;
    uint selectionWidth;
    uint selectionHeight;
    // This slot historically carried the unused maskTarget shader flag.  Keep
    // its ABI position and use bit 0 for source filtering and bit 1 for Qt's
    // integer setOpacity compatibility mode; bits 2/3 identify BGRA target/source.
    uint sourceFlags;
    float4 color;
};

bool smoothSourceEnabled() { return (sourceFlags & 1u) != 0; }
bool qtOpacityEnabled() { return (sourceFlags & 2u) != 0; }

float4 sourceAt(int2 p) {
    if (p.x < 0 || p.y < 0 || p.x >= int(sourceWidth) || p.y >= int(sourceHeight))
        return float4(0, 0, 0, 0);
    uint4 value = sourceImage.Load(int3(p, 0));
    if ((sourceFlags & 8u) != 0) value = value.bgra;
    return float4(value) / maximum;
}

// Coordinates denote source pixel coordinates.  The point sampler follows the
// existing Qt raster path: an exact half-pixel boundary resolves to the
// preceding sample (ties-to-lower), while values above the boundary resolve
// to the next sample.  This is equivalent to ceil(p + .5) - 1 and differs from
// floor(p + .5) at negative/half-tie coordinates.
float4 sourceNearest(float2 p) {
    int2 nearest = int2(ceil(p + .5) - 1);
    return sourceAt(nearest);
}

// Smooth source sampling is opt-in.  Qt's fixed-point raster scaler truncates
// interpolation weights to 1/256; retain that quantization before the four
// source loads so the opt-in path remains close to QPainter.
float4 sourceBilinear(float2 p) {
    int2 base = int2(floor(p));
    float2 f = floor((p - float2(base)) * 256) / 256;
    float4 a = sourceAt(base);
    float4 b = sourceAt(base + int2(1, 0));
    float4 c = sourceAt(base + int2(0, 1));
    float4 d = sourceAt(base + int2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float4 quantized(float4 p) {
    p.a = saturate(p.a);
    p.rgb = clamp(p.rgb, 0, p.a);
    return floor(saturate(p) * maximum + .5) / maximum;
}

float dabFactor(float coverage, float flow) {
    coverage = saturate(coverage);
    flow = saturate(flow);
    if (!qtOpacityEnabled())
        return coverage * flow;
    // QPainter's raster engine stores setOpacity() as an integer in [0, 256),
    // multiplies span coverage by that value with a >>8 truncation, then its
    // source-over operator consumes the resulting [0, 255] constant alpha.
    // Reproduce those two integer boundaries instead of using a float flow,
    // which accumulates visible differences across overlapping clone dabs.
    uint opacity256 = min(256u, uint(floor(flow * 256.0)));
    uint coverage255 = uint(floor(coverage * 255.0 + .5));
    uint effective = (coverage255 * opacity256) >> 8;
    return float(effective) / 255.0;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height)
        return;

    int2 pixel = int2(id.xy);
    uint4 input = targetImage.Load(int3(pixel, 0));
    if ((sourceFlags & 4u) != 0) input = input.bgra;
    float4 dst = float4(input) / maximum;
    const float4 original = dst;
    for (uint i = 0; i < dabCount; ++i) {
        DabInfo dab = dabs[i];
        int2 local = pixel - dab.target.xy;
        if (local.x < 0 || local.y < 0 || local.x >= dab.target.z || local.y >= dab.target.w)
            continue;
        float factor = dabFactor(coverageImage.Load(int3(dab.atlas.xy + local, 0)), dab.source.z);
        if (factor <= 0)
            continue;

        if (operation == 1) {
            // DestinationOut; RGB is premultiplied, so all four channels scale.
            dst = quantized(dst * (1 - factor));
            continue;
        }

        float4 src;
        if (operation == 0) {
            float alpha = saturate(color.a * factor);
            src = float4(color.rgb * alpha, alpha);
        } else if (hasSource) {
            const float2 sourcePoint = float2(pixel) + dab.source.xy;
            src = smoothSourceEnabled() ? sourceBilinear(sourcePoint) : sourceNearest(sourcePoint);
            src.rgb *= factor;
            src.a *= factor;
            if (qtOpacityEnabled())
                src = quantized(src);
        } else {
            src = float4(0, 0, 0, 0);
        }

        // SourceOver for premultiplied channels.  Quantize at every dab to
        // preserve 8/16-bit integer painting semantics.
        dst = quantized(src + dst * (1 - src.a));
    }
    if (hasSelection && pixel.x < int(selectionWidth) && pixel.y < int(selectionHeight)) {
        // A feathered selection constrains the completed stroke once.  Doing
        // this inside the dab loop would attenuate the same selection coverage
        // repeatedly for overlapping dabs and diverge from the documented
        // batch semantics.
        const float selection = selectionImage.Load(int3(pixel, 0));
        dst = quantized(lerp(original, dst, saturate(selection)));
    }
    uint4 result = uint4(floor(saturate(dst) * maximum + .5));
    outputImage[pixel] = (sourceFlags & 4u) != 0 ? result.bgra : result;
}
)hlsl";

} // namespace comp
