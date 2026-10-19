#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int legacy_spot_heal(uint8_t *, const uint8_t *, size_t, size_t, size_t, float, int, uint32_t);
#ifdef __cplusplus
}
namespace comp::legacy_heal {
int spot_heal(uint16_t *, const uint8_t *, size_t, size_t, size_t, float, int, uint32_t);
}
#endif
