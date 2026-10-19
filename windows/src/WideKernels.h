#pragma once
#include <cstdint>
#include <cstddef>
extern "C" {
#include "AdjustPixels.h"
#include "DitherPixels.h"
}
// All wide pixel strides count uint16_t elements, not bytes.
namespace comp::wide {


void adjust_gradient_map(uint16_t *rgba, size_t width, size_t height, size_t stride, const uint16_t *table);

void adjust_grain(uint16_t *rgba, size_t width, size_t height, size_t stride, double amount, double size,
                  double roughness, uint32_t seed, double originX, double originY, double unitsPerPixel);

void adjust_black_white(uint16_t *rgba, size_t width, size_t height, size_t stride, const float *weights,
                        int tint, double tintHue, double tintSaturation);

void adjust_color_balance(uint16_t *rgba, size_t width, size_t height, size_t stride, const float *shadows,
                          const float *midtones, const float *highlights, int preserveLuminosity);

void rgba_clamp_premultiplied(uint16_t *rgba, size_t count);



void camera_raw_compose(float *out, int grid, const CameraRawStage *stages, int count, int size);

void camera_raw_compose_onto(float *table, int grid, const CameraRawStage *stages, int count, int size);

void camera_raw_table_curves(float *table, int grid, const float *toneLut, const float *redLut, const float *greenLut,
                             const float *blueLut);

void camera_raw_scope(const uint16_t *rgba, size_t width, size_t height, size_t stride, int step, double *bins, double *scope,
                      int side);

void camera_raw_stage_color(const CameraRawStage *stage, int size, double *rgb);

void camera_raw_statistics(const uint16_t *rgba, size_t width, size_t height, size_t stride, double *out);

void adjust_camera_raw(uint16_t *rgba, size_t width, size_t height, size_t stride, const float *table, int grid,
                       int clipping);

int camera_raw_lookup_is_valid(const float *table, int grid);

void camera_raw_repair_lookup(uint16_t *result, const uint16_t *source, const uint8_t *uncertain,
                               size_t width, size_t height, size_t stride, const float *table, int grid);

void adjust_camera_raw_shadows(uint16_t *rgba, size_t width, size_t height, size_t stride, const float *coefficients,
                               int xCount, float xLow, float xStep, int kCount, float kLow, float kStep);
void adjust_camera_raw_clip_overlay(uint16_t *rgba, size_t width, size_t height, size_t stride, int shadows, int highlights);

void adjust_camera_raw_curve_color(uint16_t *rgba, size_t width, size_t height, size_t stride,
                                   const float *toneLut, const float *redLut, const float *greenLut, const float *blueLut,
                                   double refineSaturation, const float *mixer, int pointCount, const float *points,
                                   const float *grade, double blending, double balance, int visualize,
                                   const float *mixerTable, const float *gradeTable, int grid);
void adjust_camera_raw_effects(uint16_t *rgba, size_t width, size_t height, size_t stride,
                               double texture, double clarity, double dehaze,
                               double glow, int glowStyle, double glowRange, double glowSpread, double glowWarmth,
                               double vignetteAmount, double vignetteMidpoint, double vignetteRoundness,
                               double vignetteFeather, double vignetteHighlights, int vignetteStyle,
                               double scale);

void adjust_colored_vignette(uint16_t *rgba, size_t width, size_t height, size_t stride,
                             double frameX, double frameY, double frameWidth, double frameHeight, int fillsClear,
                             double amount, double midpoint, double roundness, double feather,
                             double highlights, double red, double green, double blue);

void adjust_tonal_contrast(uint16_t *rgba, const uint16_t *blurred, size_t width, size_t height,
                           size_t stride, size_t blurredStride, double amount,
                           double shadows, double midtones, double highlights);

void adjust_camera_raw_detail(uint16_t *rgba, size_t width, size_t height, size_t stride,
                              double sharpenAmount, double sharpenRadius, double sharpenDetail, double sharpenMasking,
                              double noiseLuminance, double noiseLuminanceDetail, double noiseLuminanceContrast,
                              double noiseColor, double noiseColorDetail, double noiseColorSmoothness, double scale);

void adjust_camera_raw_sharpen_mask_overlay(uint16_t *rgba, size_t width, size_t height, size_t stride,
                                            double sharpenRadius, double sharpenDetail, double sharpenMasking, double scale);

void adjust_camera_raw_optics(uint16_t *rgba, size_t width, size_t height, size_t stride,
                              int removeChromatic, int lensProfile, double profileDistortion, double profileVignetting,
                              double distortionK, double purpleAmount, double purpleHueLow, double purpleHueHigh,
                              double greenAmount, double greenHueLow, double greenHueHigh,
                              double vignetteAmount, double vignetteMidpoint, double scale,
                              const uint8_t *exposureTables, int tableSize);

void adjust_camera_raw_calibration(uint16_t *rgba, size_t width, size_t height, size_t stride,
                                   double shadowTint, double redHue, double redSaturation,
                                   double greenHue, double greenSaturation, double blueHue, double blueSaturation,
                                   int processVersion);


void lens_distort(const uint16_t *source, uint16_t *destination, size_t width, size_t height, size_t stride, double k);


void noise_add(uint16_t *rgba, size_t width, size_t height, size_t stride,
               float amount, int gaussian, int monochromatic, uint32_t seed);
void noise_add_at(uint16_t *rgba, size_t width, size_t height, size_t stride,
                  float amount, int gaussian, int monochromatic, uint32_t seed,
                  int64_t origin_x, int64_t origin_y);
int scanlines_apply(uint16_t *rgba, size_t width, size_t height, size_t stride, const ScanlinesParams *params);
void dither_glow(uint16_t *rgba, const uint16_t *glow, size_t width, size_t height, size_t stride, float amount);int spot_heal(uint16_t *, const uint8_t *, size_t, size_t, size_t, float, int, uint32_t);
int patch_fill(uint16_t *, size_t, const uint8_t *, size_t, int, int, uint32_t);
void heal_membrane(float *, const uint8_t *, long, long, int);

}
