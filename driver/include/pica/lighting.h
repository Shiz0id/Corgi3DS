/*
 * pica/lighting.h - fragment lighting register encoders.
 *
 * Thin helpers that encode the PICA200's fragment lighting registers.  They
 * deliberately add no semantics of their own: the meaning of many of these
 * fields is only partly documented, and examples/probe exists to measure it
 * on hardware.  Field layouts come from citro3d and Azahar (both public).
 *
 * Pipeline summary (as publicly understood):
 *   - The vertex shader must output a normal quaternion (rotation of +Z onto
 *     the surface normal) and a view vector (surface -> eye, view space).
 *   - For each enabled light: diffuse = diffuse_color * max(L.N, 0) + ambient,
 *     specular = spec0 * D0(lut) + spec1 * D1(lut) * reflectance(lut), both
 *     scaled by optional spot and distance attenuation LUTs.
 *   - The sums are available to the texture combiners as
 *     PICA_SRC_FRAGMENT_PRIMARY_COLOR (diffuse) and
 *     PICA_SRC_FRAGMENT_SECONDARY_COLOR (specular).
 */
#ifndef PICA_LIGHTING_H
#define PICA_LIGHTING_H

#include "pica.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LUT identifiers (the LUT_INDEX "type" field) */
#define PICA_LUT_D0     0
#define PICA_LUT_D1     1
#define PICA_LUT_FR     3
#define PICA_LUT_RB     4
#define PICA_LUT_RG     5
#define PICA_LUT_RR     6
#define PICA_LUT_SP(n)  (8 + (n))   /* spotlight attenuation, light n */
#define PICA_LUT_DA(n)  (16 + (n))  /* distance attenuation, light n */

/* Slot of a common LUT in the LUTINPUT_ABS/SELECT/SCALE registers */
typedef enum
{
    PICA_LUTSLOT_D0 = 0,
    PICA_LUTSLOT_D1 = 1,
    PICA_LUTSLOT_SP = 2,
    PICA_LUTSLOT_FR = 3,
    PICA_LUTSLOT_RB = 4,
    PICA_LUTSLOT_RG = 5,
    PICA_LUTSLOT_RR = 6,
} pica_lut_slot_t;

typedef enum
{
    PICA_LUTINPUT_NH = 0, /* normal . half vector */
    PICA_LUTINPUT_VH = 1, /* view . half vector */
    PICA_LUTINPUT_NV = 2, /* normal . view */
    PICA_LUTINPUT_LN = 3, /* light . normal */
    PICA_LUTINPUT_SP = 4, /* -light . spot direction */
    PICA_LUTINPUT_CP = 5, /* cosine of phi (tangent space) */
} pica_lut_input_t;

typedef enum
{
    PICA_LUTSCALE_1X   = 0,
    PICA_LUTSCALE_2X   = 1,
    PICA_LUTSCALE_4X   = 2,
    PICA_LUTSCALE_8X   = 3,
    PICA_LUTSCALE_0_25 = 6,
    PICA_LUTSCALE_0_5  = 7,
} pica_lut_scale_t;

/* CONFIG1 disable bits */
#define PICA_LC1_SHADOW_OFF(n)  (1u << (n))
#define PICA_LC1_SPOT_OFF(n)    (1u << ((n) + 8))
#define PICA_LC1_LUT_OFF(lut)   (1u << ((lut) + 16))   /* lut = PICA_LUT_D0..RR */
#define PICA_LC1_DIST_OFF(n)    (1u << ((n) + 24))

/* CONFIG0 fields */
#define PICA_LC0_SHADOW_ENABLE      (1u << 0)
#define PICA_LC0_FRESNEL_PRIMARY    (1u << 2)
#define PICA_LC0_FRESNEL_SECONDARY  (1u << 3)
#define PICA_LC0_LAYER(n)           (((uint32_t)(n) & 0xF) << 4)
#define PICA_LC0_CLAMP_HIGHLIGHTS   (1u << 27)

/* Per-light CONFIG bits */
#define PICA_LIGHT_DIRECTIONAL      (1u << 0)
#define PICA_LIGHT_TWO_SIDED        (1u << 1)
#define PICA_LIGHT_GEO_FACTOR0      (1u << 2)
#define PICA_LIGHT_GEO_FACTOR1      (1u << 3)

typedef struct pica_light_env
{
    uint32_t ambient;      /* LIGHTING_AMBIENT, see pica_light_color() */
    uint32_t num_lights;   /* 1-8 */
    uint32_t config0;
    uint32_t config1;
    uint32_t lut_abs;      /* LUTINPUT_ABS: bit 4*slot+1 set = signed input (no abs) */
    uint32_t lut_select;   /* LUTINPUT_SELECT: 4 bits per slot */
    uint32_t lut_scale;    /* LUTINPUT_SCALE: 4 bits per slot */
    uint32_t permutation;  /* slot n -> light id, 4 bits per slot */
} pica_light_env;

typedef struct pica_light
{
    uint32_t specular0, specular1, diffuse, ambient;  /* pica_light_color() */
    uint32_t xy, z;          /* position/direction as f16 */
    uint32_t spot_xy, spot_z;/* spot direction, signed 1.1.11 fixed point */
    uint32_t config;         /* PICA_LIGHT_* */
    uint32_t dist_bias, dist_scale; /* f20 */
} pica_light;

/* 8-bit RGB in the 10-bit-per-channel register layout (B low, R high). */
static inline uint32_t pica_light_color8(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint32_t)b | ((uint32_t)g << 10) | ((uint32_t)r << 20);
}
uint32_t pica_light_color(float r, float g, float b);

/* LUT entry: value in [0, 1] (12-bit), diff in [-1, 1] (sign + 11 bits). */
uint32_t pica_lut_entry_raw(uint32_t value12, int32_t diff11);
uint32_t pica_lut_entry(float value, float diff);

/* Environment with every light/LUT feature disabled, layer config 0, one light (id 0). */
void pica_light_env_init(pica_light_env* env);
void pica_light_env_lut(pica_light_env* env, pica_lut_slot_t slot, pica_lut_input_t input, bool abs_input,
                        pica_lut_scale_t scale);

void pica_light_init(pica_light* light);
void pica_light_position(pica_light* light, float x, float y, float z, bool directional);
void pica_light_spot_dir(pica_light* light, float x, float y, float z);
void pica_light_dist_atten(pica_light* light, float bias, float scale);

/* Turn fragment lighting on/off. */
void pica_lighting_enable(pica_context* ctx, bool enable);
void pica_light_env_bind(pica_context* ctx, const pica_light_env* env);
void pica_light_bind(pica_context* ctx, unsigned id, const pica_light* light);
/* Upload 256 entries to a LUT (PICA_LUT_*). */
void pica_lut_upload(pica_context* ctx, unsigned lut, const uint32_t entries[256]);

#ifdef __cplusplus
}
#endif

#endif /* PICA_LIGHTING_H */
