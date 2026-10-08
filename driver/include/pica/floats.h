/*
 * pica/floats.h - conversions to the PICA200's reduced-precision floats.
 *
 *   f16: 1 sign,  5 exponent (bias 15), 10 mantissa
 *   f20: 1 sign,  7 exponent (bias 63), 12 mantissa
 *   f24: 1 sign,  7 exponent (bias 63), 16 mantissa  (shader / vertex math)
 *   f31: 1 sign,  7 exponent (bias 63), 23 mantissa
 *
 * Denormals flush to zero and out-of-range values saturate to infinity.
 */
#ifndef PICA_FLOATS_H
#define PICA_FLOATS_H

#include <stdint.h>
#include <string.h>

static inline uint32_t pica__f32_bits(float f)
{
    uint32_t i;
    memcpy(&i, &f, sizeof(i));
    return i;
}

static inline uint32_t pica__f32_to_small(float f, int exp_bits, int mant_bits, int bias)
{
    uint32_t i = pica__f32_bits(f);
    uint32_t sign = i >> 31;
    int32_t exponent = (int32_t)((i >> 23) & 0xFF);
    uint32_t mantissa = (i & 0x7FFFFF) >> (23 - mant_bits);
    uint32_t total = 1 + exp_bits + mant_bits;
    uint32_t exp_max = (1u << exp_bits) - 1;

    if (exponent == 0)
        return sign << (total - 1);
    exponent = exponent - 127 + bias;
    if (exponent <= 0)
        return sign << (total - 1);
    if ((uint32_t)exponent >= exp_max)
        return (sign << (total - 1)) | (exp_max << mant_bits);
    return (sign << (total - 1)) | ((uint32_t)exponent << mant_bits) | mantissa;
}

static inline uint32_t pica_f32_to_f16(float f) { return pica__f32_to_small(f, 5, 10, 15); }
static inline uint32_t pica_f32_to_f20(float f) { return pica__f32_to_small(f, 7, 12, 63); }
static inline uint32_t pica_f32_to_f24(float f) { return pica__f32_to_small(f, 7, 16, 63); }
static inline uint32_t pica_f32_to_f31(float f) { return pica__f32_to_small(f, 7, 23, 63); }

/* Pack four f24 values into the 3-word layout used by float uniform and
 * fixed attribute uploads: word0 = w:z[23:16], word1 = z[15:0]:y[23:8],
 * word2 = y[7:0]:x. */
static inline void pica_pack_f24x4(uint32_t out[3], float x, float y, float z, float w)
{
    uint32_t fx = pica_f32_to_f24(x), fy = pica_f32_to_f24(y);
    uint32_t fz = pica_f32_to_f24(z), fw = pica_f32_to_f24(w);
    out[0] = (fw << 8) | (fz >> 16);
    out[1] = ((fz & 0xFFFF) << 16) | (fy >> 8);
    out[2] = ((fy & 0xFF) << 24) | fx;
}

/* Signed 1.3.8 fixed point, used by the texture LOD bias. */
static inline uint32_t pica_f32_to_fixed13(float f)
{
    int32_t v = (int32_t)(f * 256.0f);
    if (v > 0xFFF) v = 0xFFF;
    if (v < -0x1000) v = -0x1000;
    return (uint32_t)v & 0x1FFF;
}

#endif /* PICA_FLOATS_H */
