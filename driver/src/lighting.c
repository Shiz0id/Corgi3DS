/*
 * lighting.c - fragment lighting register encoders.
 */
#include <math.h>
#include <string.h>
#include "internal.h"
#include "pica/lighting.h"

static uint32_t to8(float v)
{
    if (!(v > 0.0f))
        return 0;
    if (v >= 1.0f)
        return 255;
    return (uint32_t)(v * 255.0f + 0.5f);
}

uint32_t pica_light_color(float r, float g, float b)
{
    return pica_light_color8((uint8_t)to8(r), (uint8_t)to8(g), (uint8_t)to8(b));
}

uint32_t pica_lut_entry_raw(uint32_t value12, int32_t diff11)
{
    uint32_t mag = (uint32_t)(diff11 < 0 ? -diff11 : diff11);
    if (mag > 0x7FF)
        mag = 0x7FF;
    return (value12 & 0xFFF) | (mag << 12) | (diff11 < 0 ? (1u << 23) : 0);
}

uint32_t pica_lut_entry(float value, float diff)
{
    uint32_t v = value <= 0.0f ? 0 : (value >= 1.0f ? 0xFFF : (uint32_t)(value * 4096.0f));
    if (v > 0xFFF)
        v = 0xFFF;
    float d = diff * 2048.0f;
    int32_t di = (int32_t)(d < 0 ? d - 0.5f : d + 0.5f);
    return pica_lut_entry_raw(v, di);
}

void pica_light_env_init(pica_light_env* env)
{
    memset(env, 0, sizeof(*env));
    env->num_lights = 1;
    env->config0 = PICA_LC0_LAYER(0);
    env->config1 = 0xFFFFFFFFu; /* everything off */
    env->lut_abs = 0;           /* abs() on all inputs */
    env->permutation = 0x76543210;
}

void pica_light_env_lut(pica_light_env* env, pica_lut_slot_t slot, pica_lut_input_t input, bool abs_input,
                        pica_lut_scale_t scale)
{
    unsigned sh = (unsigned)slot * 4;
    env->lut_select = (env->lut_select & ~(0xFu << sh)) | ((uint32_t)input << sh);
    env->lut_scale = (env->lut_scale & ~(0xFu << sh)) | ((uint32_t)scale << sh);
    env->lut_abs &= ~(1u << (sh + 1));
    if (!abs_input)
        env->lut_abs |= 1u << (sh + 1);
}

void pica_light_init(pica_light* light)
{
    memset(light, 0, sizeof(*light));
    light->diffuse = pica_light_color8(255, 255, 255);
    light->specular0 = pica_light_color8(255, 255, 255);
    light->specular1 = pica_light_color8(255, 255, 255);
    pica_light_position(light, 0, 0, 1, true);
}

void pica_light_position(pica_light* light, float x, float y, float z, bool directional)
{
    light->xy = pica_f32_to_f16(x) | (pica_f32_to_f16(y) << 16);
    light->z = pica_f32_to_f16(z);
    light->config &= ~PICA_LIGHT_DIRECTIONAL;
    if (directional)
        light->config |= PICA_LIGHT_DIRECTIONAL;
}

static uint32_t fix2_11(float v)
{
    int32_t i = (int32_t)(v * 2048.0f);
    if (i > 0xFFF)
        i = 0xFFF;
    if (i < -0x1000)
        i = -0x1000;
    return (uint32_t)i & 0x1FFF;
}

void pica_light_spot_dir(pica_light* light, float x, float y, float z)
{
    float len = sqrtf(x * x + y * y + z * z);
    if (len > 0.0f)
    {
        x /= len;
        y /= len;
        z /= len;
    }
    /* The hardware wants the direction pointing back towards the light */
    light->spot_xy = fix2_11(-x) | (fix2_11(-y) << 16);
    light->spot_z = fix2_11(-z);
}

void pica_light_dist_atten(pica_light* light, float bias, float scale)
{
    light->dist_bias = pica_f32_to_f20(bias);
    light->dist_scale = pica_f32_to_f20(scale);
}

void pica_lighting_enable(pica_context* ctx, bool enable)
{
    if (!pica__reserve(ctx, 4))
        return;
    pica__write(ctx, PICA_REG_LIGHTING_ENABLE0, enable ? 1 : 0);
    pica__write(ctx, PICA_REG_LIGHTING_ENABLE1, enable ? 0 : 1);
}

void pica_light_env_bind(pica_context* ctx, const pica_light_env* env)
{
    uint32_t v[3];

    if (!env || env->num_lights < 1 || env->num_lights > 8)
    {
        pica__set_error(ctx, PICA_ERR_ARG);
        return;
    }
    if (!pica__reserve(ctx, 2 + pica__cmd_words(3) * 2 + 2))
        return;
    pica__write(ctx, PICA_REG_LIGHTING_AMBIENT, env->ambient);
    v[0] = env->num_lights - 1;
    v[1] = env->config0;
    v[2] = env->config1;
    pica__write_inc(ctx, PICA_REG_LIGHTING_NUM_LIGHTS, v, 3);
    v[0] = env->lut_abs;
    v[1] = env->lut_select;
    v[2] = env->lut_scale;
    pica__write_inc(ctx, PICA_REG_LIGHTING_LUTINPUT_ABS, v, 3);
    pica__write(ctx, PICA_REG_LIGHTING_LIGHT_PERMUTATION, env->permutation);
}

void pica_light_bind(pica_context* ctx, unsigned id, const pica_light* light)
{
    uint32_t v[12];

    if (id > 7 || !light)
    {
        pica__set_error(ctx, PICA_ERR_ARG);
        return;
    }
    v[0] = light->specular0;
    v[1] = light->specular1;
    v[2] = light->diffuse;
    v[3] = light->ambient;
    v[4] = light->xy;
    v[5] = light->z;
    v[6] = light->spot_xy;
    v[7] = light->spot_z;
    v[8] = 0;
    v[9] = light->config;
    v[10] = light->dist_bias;
    v[11] = light->dist_scale;
    if (pica__reserve(ctx, pica__cmd_words(12)))
        pica__write_inc(ctx, (uint16_t)(PICA_REG_LIGHT0_SPECULAR0 + id * 0x10), v, 12);
}

void pica_lut_upload(pica_context* ctx, unsigned lut, const uint32_t entries[256])
{
    if (lut > 23 || !entries)
    {
        pica__set_error(ctx, PICA_ERR_ARG);
        return;
    }
    if (!pica__reserve(ctx, 2 + pica__cmd_words(256)))
        return;
    pica__write(ctx, PICA_REG_LIGHTING_LUT_INDEX, (uint32_t)lut << 8);
    pica__write_rep(ctx, PICA_REG_LIGHTING_LUT_DATA0, entries, 256);
}
