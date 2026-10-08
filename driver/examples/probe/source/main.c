/*
 * PICA200 hardware probe.
 *
 * Runs parameter sweeps against the GPU and records exactly what comes out,
 * so undocumented behaviour can be measured instead of guessed.  Each
 * experiment draws a row of 1-pixel-wide samples into a 256x16 render
 * target; sample i gets its own geometry and (optionally) its own GPU
 * state.  Every sample is rendered twice: once showing the fragment
 * lighting's primary (diffuse) output and once its secondary (specular)
 * output.
 *
 * Results go to sdmc:/pica_probe/:
 *   meta.txt           console model / firmware, probe version
 *   <experiment>.csv   one row per sample: parameters, then
 *                      pr,pg,pb,pa (primary) and sr,sg,sb,sa (secondary)
 *
 * Compare two result sets (e.g. hardware vs an emulator) with
 * driver/tools/probe_compare.py.
 */
#include <3ds.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "pica/pica.h"
#include "pica/lighting.h"
#include "lit_shbin.h"

#define PROBE_VERSION 1
#define OUT_DIR "sdmc:/pica_probe"
#define W 256
#define H 16
#define MAX_SAMPLES 256

typedef struct
{
    float pos[3];
    float quat[4];
    float view[3];
} vertex;

typedef struct probe probe;
typedef void (*setup_fn)(probe* P, int i);

struct probe
{
    pica_context ctx;
    pica_framebuffer fb;
    pica_shbin shbin;
    pica_attr_info attrs;
    pica_buf_info bufs;
    vertex* vbo;
    uint32_t* rb;

    /* Per-sample geometry, filled in by experiments */
    float quat[MAX_SAMPLES][4];
    float view[MAX_SAMPLES][3];

    /* Results */
    uint32_t primary[MAX_SAMPLES];
    uint32_t secondary[MAX_SAMPLES];

    /* Current experiment */
    FILE* csv;
    int failures;
};

static probe P_;

/* ---- Output ------------------------------------------------------------------ */

static void say(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static void csv_begin(probe* P, const char* name, const char* param_header)
{
    char path[96];
    snprintf(path, sizeof(path), OUT_DIR "/%s.csv", name);
    P->csv = fopen(path, "w");
    if (P->csv)
        fprintf(P->csv, "sample,%s,pr,pg,pb,pa,sr,sg,sb,sa\n", param_header);
    say("%-22s", name);
}

static void csv_row(probe* P, int i, const char* fmt, ...)
{
    if (!P->csv)
        return;
    va_list ap;
    fprintf(P->csv, "%d,", i);
    va_start(ap, fmt);
    vfprintf(P->csv, fmt, ap);
    va_end(ap);
    uint32_t p = P->primary[i], s = P->secondary[i];
    fprintf(P->csv, ",%u,%u,%u,%u,%u,%u,%u,%u\n", (unsigned)(p >> 24), (unsigned)((p >> 16) & 0xFF),
            (unsigned)((p >> 8) & 0xFF), (unsigned)(p & 0xFF), (unsigned)(s >> 24),
            (unsigned)((s >> 16) & 0xFF), (unsigned)((s >> 8) & 0xFF), (unsigned)(s & 0xFF));
}

static void csv_end(probe* P)
{
    int err = pica_get_error(&P->ctx);
    if (P->csv)
        fclose(P->csv);
    P->csv = NULL;
    if (err)
        P->failures++;
    say(err ? "driver error %d\n" : "done\n", err);
}

/* ---- Geometry helpers -------------------------------------------------------------- */

/* Quaternion rotating +Z onto the normal (sin t, 0, cos t) */
static void quat_y(float q[4], float theta)
{
    q[0] = 0.0f;
    q[1] = sinf(theta * 0.5f);
    q[2] = 0.0f;
    q[3] = cosf(theta * 0.5f);
}

static void set_all(probe* P, int n, float theta, float vx, float vy, float vz)
{
    for (int i = 0; i < n; i++)
    {
        quat_y(P->quat[i], theta);
        P->view[i][0] = vx;
        P->view[i][1] = vy;
        P->view[i][2] = vz;
    }
}

/* Normal angle that makes N.Z (and so N.H for light = view = +Z) equal c */
static float theta_for_cos(float c)
{
    if (c > 1.0f) c = 1.0f;
    if (c < -1.0f) c = -1.0f;
    return acosf(c);
}

/* ---- Rendering --------------------------------------------------------------------- */

static void fill_vbo(probe* P, int n)
{
    for (int i = 0; i < n; i++)
    {
        float x0 = -1.0f + 2.0f * i / n, x1 = -1.0f + 2.0f * (i + 1) / n;
        const float xy[4][2] = { { x0, -1 }, { x1, -1 }, { x0, 1 }, { x1, 1 } };
        for (int k = 0; k < 4; k++)
        {
            vertex* v = &P->vbo[i * 4 + k];
            v->pos[0] = xy[k][0];
            v->pos[1] = xy[k][1];
            v->pos[2] = -0.5f;
            memcpy(v->quat, P->quat[i], sizeof(v->quat));
            memcpy(v->view, P->view[i], sizeof(v->view));
        }
    }
}

static void select_output(probe* P, bool secondary)
{
    pica_texenv env;
    pica_texenv_init(&env);
    pica_texenv_src(&env, PICA_TEV_BOTH,
                    secondary ? PICA_SRC_FRAGMENT_SECONDARY_COLOR : PICA_SRC_FRAGMENT_PRIMARY_COLOR, 0, 0);
    pica_set_texenv(&P->ctx, 0, &env);
}

/* Draw n samples (n must divide W), calling setup(P, i) before each, for
 * both outputs, and read the results back. */
static void run(probe* P, int n, setup_fn setup)
{
    fill_vbo(P, n);
    for (int pass = 0; pass < 2; pass++)
    {
        pica_bind_framebuffer(&P->ctx, &P->fb);
        pica_clear(&P->ctx, &P->fb, PICA_CLEAR_COLOR, 0x00000000, 0);
        pica_bind_shader(&P->ctx, &P->shbin.dvle[0], NULL, 0);
        pica_bind_attrs(&P->ctx, &P->attrs);
        pica_bind_buffers(&P->ctx, &P->bufs);
        pica_blend(&P->ctx, PICA_BLEND_ADD, PICA_BLEND_ADD, PICA_ONE, PICA_ZERO, PICA_ONE, PICA_ZERO);
        pica_lighting_enable(&P->ctx, true);
        select_output(P, pass == 1);
        for (int i = 0; i < n; i++)
        {
            setup(P, i);
            pica_draw_arrays(&P->ctx, PICA_TRIANGLE_STRIP, i * 4, 4);
        }
        pica_transfer(&P->ctx, &P->fb, P->rb, W, H, PICA_XFER_RGBA8, 0);
        int cw = W / n;
        for (int i = 0; i < n; i++)
        {
            /* middle row of the target, middle of the sample's column */
            uint32_t c = P->rb[(H / 2) * W + i * cw + cw / 2];
            if (pass == 0)
                P->primary[i] = c;
            else
                P->secondary[i] = c;
        }
    }
    pica_lighting_enable(&P->ctx, false);
}

/* ---- Shared state builders --------------------------------------------------------------- */

static uint32_t lut_buf[256];

static void lut_const(uint32_t entry)
{
    for (int i = 0; i < 256; i++)
        lut_buf[i] = entry;
}

/* Entry i = i * step (12-bit value), no interpolation slope. */
static void lut_ramp(uint32_t step)
{
    for (int i = 0; i < 256; i++)
        lut_buf[i] = pica_lut_entry_raw((uint32_t)i * step, 0);
}

/* One white directional light along +Z; diffuse only unless changed */
static void basic_light(pica_light* l, bool diffuse, bool spec0, bool spec1)
{
    pica_light_init(l);
    l->diffuse = diffuse ? pica_light_color8(255, 255, 255) : 0;
    l->specular0 = spec0 ? pica_light_color8(255, 255, 255) : 0;
    l->specular1 = spec1 ? pica_light_color8(255, 255, 255) : 0;
    l->ambient = 0;
    pica_light_position(l, 0, 0, 1, true);
}

/* ===========================================================================
 * Experiments
 * ======================================================================== */

/* --- diffuse_ln: diffuse term vs N.L ------------------------------------- */

static void setup_none(probe* P, int i) { (void)P; (void)i; }

static void exp_diffuse_ln(probe* P)
{
    pica_light_env env;
    pica_light l;
    pica_light_env_init(&env);
    pica_light_env_bind(&P->ctx, &env);
    basic_light(&l, true, false, false);
    pica_light_bind(&P->ctx, 0, &l);

    for (int i = 0; i < 256; i++)
    {
        quat_y(P->quat[i], (float)M_PI * i / 255.0f);
        P->view[i][0] = 0; P->view[i][1] = 0; P->view[i][2] = 1;
    }
    csv_begin(P, "diffuse_ln", "theta_deg,cos");
    run(P, 256, setup_none);
    for (int i = 0; i < 256; i++)
        csv_row(P, i, "%.4f,%.6f", 180.0 * i / 255.0, cos(M_PI * i / 255.0));
    csv_end(P);
}

/* --- diffuse_two_sided: same sweep with the two-sided diffuse bit -------- */

static void exp_diffuse_two_sided(probe* P)
{
    pica_light_env env;
    pica_light l;
    pica_light_env_init(&env);
    pica_light_env_bind(&P->ctx, &env);
    basic_light(&l, true, false, false);
    l.config |= PICA_LIGHT_TWO_SIDED;
    pica_light_bind(&P->ctx, 0, &l);

    for (int i = 0; i < 64; i++)
    {
        quat_y(P->quat[i], (float)M_PI * i / 63.0f);
        P->view[i][0] = 0; P->view[i][1] = 0; P->view[i][2] = 1;
    }
    csv_begin(P, "diffuse_two_sided", "theta_deg,cos");
    run(P, 64, setup_none);
    for (int i = 0; i < 64; i++)
        csv_row(P, i, "%.4f,%.6f", 180.0 * i / 63.0, cos(M_PI * i / 63.0));
    csv_end(P);
}

/* --- lut_value: how a 12-bit LUT value maps to 8-bit output ------------- */

static uint32_t value_list[256];

static void setup_lut_value(probe* P, int i)
{
    lut_const(pica_lut_entry_raw(value_list[i], 0));
    pica_lut_upload(&P->ctx, PICA_LUT_D0, lut_buf);
}

static void lut_value_common(probe* P, const char* name)
{
    pica_light_env env;
    pica_light l;
    pica_light_env_init(&env);
    env.config1 &= ~PICA_LC1_LUT_OFF(PICA_LUT_D0);
    pica_light_env_lut(&env, PICA_LUTSLOT_D0, PICA_LUTINPUT_NH, true, PICA_LUTSCALE_1X);
    pica_light_env_bind(&P->ctx, &env);
    basic_light(&l, false, true, false);
    pica_light_bind(&P->ctx, 0, &l);

    set_all(P, 256, 0.5f, 0, 0, 1);
    csv_begin(P, name, "value12");
    run(P, 256, setup_lut_value);
    for (int i = 0; i < 256; i++)
        csv_row(P, i, "%u", (unsigned)value_list[i]);
    csv_end(P);
}

static void exp_lut_value(probe* P)
{
    for (int i = 0; i < 256; i++)
        value_list[i] = (uint32_t)i * 16;
    lut_value_common(P, "lut_value");
    /* Fine steps at the top and bottom of the range */
    for (int i = 0; i < 128; i++)
        value_list[i] = (uint32_t)i;
    for (int i = 0; i < 128; i++)
        value_list[128 + i] = 0xF80u + (uint32_t)i;
    lut_value_common(P, "lut_value_ends");
}

/* --- lut_index: which LUT entry an input value selects ------------------- */

static void lut_index_common(probe* P, const char* name, bool abs_input, float c0, float c1)
{
    pica_light_env env;
    pica_light l;
    pica_light_env_init(&env);
    env.config1 &= ~PICA_LC1_LUT_OFF(PICA_LUT_D0);
    pica_light_env_lut(&env, PICA_LUTSLOT_D0, PICA_LUTINPUT_NH, abs_input, PICA_LUTSCALE_1X);
    pica_light_env_bind(&P->ctx, &env);
    basic_light(&l, false, true, false);
    pica_light_bind(&P->ctx, 0, &l);
    /* entry i outputs ~i/256 (in 8-bit output, ~i) */
    lut_ramp(16);
    pica_lut_upload(&P->ctx, PICA_LUT_D0, lut_buf);

    float cs[256];
    for (int i = 0; i < 256; i++)
    {
        cs[i] = c0 + (c1 - c0) * i / 255.0f;
        quat_y(P->quat[i], theta_for_cos(cs[i]));
        /* light = view = +Z, so H = +Z and N.H = cos(theta) */
        P->view[i][0] = 0; P->view[i][1] = 0; P->view[i][2] = 1;
    }
    csv_begin(P, name, "n_dot_h");
    run(P, 256, setup_none);
    for (int i = 0; i < 256; i++)
        csv_row(P, i, "%.6f", cs[i]);
    csv_end(P);
}

static void exp_lut_index(probe* P)
{
    lut_index_common(P, "lut_index_abs", true, 0.0f, 1.0f);
    /* zoom: N.H around 0.5 at sub-entry resolution */
    lut_index_common(P, "lut_index_abs_zoom", true, 0.49f, 0.51f);
    /* signed inputs: negative N.H indexes the LUT as a two's complement byte? */
    lut_index_common(P, "lut_index_signed", false, -1.0f, 1.0f);
}

/* --- lut_interp: does the diff field interpolate between entries? ------- */

static void lut_interp_common(probe* P, const char* name, uint32_t value, int32_t diff)
{
    pica_light_env env;
    pica_light l;
    pica_light_env_init(&env);
    env.config1 &= ~PICA_LC1_LUT_OFF(PICA_LUT_D0);
    pica_light_env_lut(&env, PICA_LUTSLOT_D0, PICA_LUTINPUT_NH, true, PICA_LUTSCALE_1X);
    pica_light_env_bind(&P->ctx, &env);
    basic_light(&l, false, true, false);
    pica_light_bind(&P->ctx, 0, &l);
    lut_const(pica_lut_entry_raw(value, diff));
    pica_lut_upload(&P->ctx, PICA_LUT_D0, lut_buf);

    float cs[256];
    for (int i = 0; i < 256; i++)
    {
        /* four entries' worth of input, 64 samples per entry */
        cs[i] = (128.0f + i / 64.0f) / 256.0f;
        quat_y(P->quat[i], theta_for_cos(cs[i]));
        P->view[i][0] = 0; P->view[i][1] = 0; P->view[i][2] = 1;
    }
    csv_begin(P, name, "n_dot_h");
    run(P, 256, setup_none);
    for (int i = 0; i < 256; i++)
        csv_row(P, i, "%.6f", cs[i]);
    csv_end(P);
}

static void exp_lut_interp(probe* P)
{
    lut_interp_common(P, "lut_interp_pos", 0, 0x7FF);
    lut_interp_common(P, "lut_interp_neg", 0xFFF, -0x7FF);
    lut_interp_common(P, "lut_interp_half", 0x800, 0x400);
}

/* --- lut_scale: all 8 values of a scale selector ------------------------ */

static void setup_lut_scale(probe* P, int i)
{
    pica_light_env env;
    pica_light_env_init(&env);
    env.config1 &= ~PICA_LC1_LUT_OFF(PICA_LUT_D0);
    pica_light_env_lut(&env, PICA_LUTSLOT_D0, PICA_LUTINPUT_NH, true, (pica_lut_scale_t)(i / 4));
    pica_light_env_bind(&P->ctx, &env);
}

static void exp_lut_scale(probe* P)
{
    pica_light l;
    basic_light(&l, false, true, false);
    pica_light_bind(&P->ctx, 0, &l);
    lut_const(pica_lut_entry_raw(0x200, 0)); /* 0.125 */
    pica_lut_upload(&P->ctx, PICA_LUT_D0, lut_buf);

    set_all(P, 32, 0.5f, 0, 0, 1);
    csv_begin(P, "lut_scale", "selector");
    run(P, 32, setup_lut_scale); /* 4 samples per selector */
    for (int i = 0; i < 32; i++)
        csv_row(P, i, "%d", i / 4);
    csv_end(P);
}

/* --- layer_config / config bit sweeps ---------------------------------------
 * A scene that uses every common LUT with a distinct constant, so the
 * output identifies which LUTs a configuration actually applies. */

static pica_light_env layer_env;

static void layer_scene(probe* P)
{
    pica_light l;
    static const struct { int lut; uint32_t value; } luts[] = {
        { PICA_LUT_D0, 0x400 }, /* 0.25 */
        { PICA_LUT_D1, 0x800 }, /* 0.5 */
        { PICA_LUT_FR, 0x666 }, /* 0.4 */
        { PICA_LUT_RB, 0x400 }, /* 0.25 */
        { PICA_LUT_RG, 0xC00 }, /* 0.75 */
        { PICA_LUT_RR, 0xFFF }, /* 1.0 */
        { PICA_LUT_SP(0), 0xE00 }, /* 0.875 */
        { PICA_LUT_DA(0), 0xF00 }, /* 0.9375 */
    };
    for (unsigned k = 0; k < sizeof(luts) / sizeof(luts[0]); k++)
    {
        lut_const(pica_lut_entry_raw(luts[k].value, 0));
        pica_lut_upload(&P->ctx, (unsigned)luts[k].lut, lut_buf);
    }

    pica_light_env_init(&layer_env);
    layer_env.config0 = PICA_LC0_LAYER(4) | PICA_LC0_FRESNEL_PRIMARY | PICA_LC0_FRESNEL_SECONDARY;
    layer_env.config1 = 0; /* everything on */
    layer_env.ambient = pica_light_color8(0x10, 0x10, 0x10);
    for (int s = 0; s <= PICA_LUTSLOT_RR; s++)
        pica_light_env_lut(&layer_env, (pica_lut_slot_t)s, PICA_LUTINPUT_NH, true, PICA_LUTSCALE_1X);

    basic_light(&l, true, true, true);
    /* spec0 red only, so R = D0*1 + D1*RR, G = D1*RG, B = D1*RB */
    l.specular0 = pica_light_color8(255, 0, 0);
    l.diffuse = pica_light_color8(0x80, 0x80, 0x80);
    pica_light_bind(&P->ctx, 0, &l);
}

static void setup_layer(probe* P, int i)
{
    pica_light_env env = layer_env;
    env.config0 = (env.config0 & ~(0xFu << 4)) | PICA_LC0_LAYER(i / 2);
    pica_light_env_bind(&P->ctx, &env);
}

static void setup_config0_bit(probe* P, int i)
{
    pica_light_env env = layer_env;
    if (i > 0 && i <= 32)
        env.config0 ^= 1u << (i - 1);
    pica_light_env_bind(&P->ctx, &env);
}

static void setup_config1_bit(probe* P, int i)
{
    pica_light_env env = layer_env;
    if (i > 0 && i <= 32)
        env.config1 ^= 1u << (i - 1);
    pica_light_env_bind(&P->ctx, &env);
}

static void exp_layers(probe* P)
{
    /* normal tilted 30 degrees so LN/NH-type inputs are not all 1 */
    layer_scene(P);
    set_all(P, 32, (float)M_PI / 6.0f, 0, 0, 1);
    csv_begin(P, "layer_config", "layer");
    run(P, 32, setup_layer); /* 2 samples per layer value 0..15 */
    for (int i = 0; i < 32; i++)
        csv_row(P, i, "%d", i / 2);
    csv_end(P);

    /* sample 0 = baseline, sample k = baseline with bit k-1 flipped */
    layer_scene(P);
    set_all(P, 64, (float)M_PI / 6.0f, 0, 0, 1);
    csv_begin(P, "config0_bits", "flipped_bit");
    run(P, 64, setup_config0_bit);
    for (int i = 0; i < 33; i++)
        csv_row(P, i, "%d", i - 1);
    csv_end(P);

    layer_scene(P);
    csv_begin(P, "config1_bits", "flipped_bit");
    run(P, 64, setup_config1_bit);
    for (int i = 0; i < 33; i++)
        csv_row(P, i, "%d", i - 1);
    csv_end(P);
}

/* --- light_config_bits: per-light CONFIG register, all 32 bits ----------- */

static void setup_light_bit(probe* P, int i)
{
    pica_light l;
    basic_light(&l, true, true, false);
    l.diffuse = pica_light_color8(0x80, 0x80, 0x80);
    l.ambient = pica_light_color8(0x10, 0x10, 0x10);
    /* positional light up and to the side, so directional vs positional differ */
    pica_light_position(&l, 0.5f, 0.5f, 0.5f, false);
    if (i > 0 && i <= 32)
        l.config ^= 1u << (i - 1);
    pica_light_bind(&P->ctx, 0, &l);
}

static void exp_light_bits(probe* P)
{
    pica_light_env env;
    pica_light_env_init(&env);
    env.config1 &= ~PICA_LC1_LUT_OFF(PICA_LUT_D0);
    pica_light_env_lut(&env, PICA_LUTSLOT_D0, PICA_LUTINPUT_NH, true, PICA_LUTSCALE_1X);
    pica_light_env_bind(&P->ctx, &env);
    lut_ramp(16);
    pica_lut_upload(&P->ctx, PICA_LUT_D0, lut_buf);

    /* back-facing-ish normal (120 degrees) so two-sided/clamping bits show */
    set_all(P, 64, 2.0f * (float)M_PI / 3.0f, 0, 0, 1);
    csv_begin(P, "light_config_bits", "flipped_bit");
    run(P, 64, setup_light_bit);
    for (int i = 0; i < 33; i++)
        csv_row(P, i, "%d", i - 1);
    csv_end(P);
}

/* --- dist_atten: distance attenuation LUT addressing --------------------- */

static float dist_list[256];

static void setup_dist(probe* P, int i)
{
    pica_light l;
    basic_light(&l, true, false, false);
    /* view = +Z, light vector = position + view = (0, 0, d) */
    pica_light_position(&l, 0, 0, dist_list[i] - 1.0f, false);
    pica_light_dist_atten(&l, 0.0f, 1.0f / 8.0f);
    pica_light_bind(&P->ctx, 0, &l);
}

static void exp_dist_atten(probe* P)
{
    pica_light_env env;
    pica_light_env_init(&env);
    env.config1 &= ~PICA_LC1_DIST_OFF(0);
    pica_light_env_bind(&P->ctx, &env);
    lut_ramp(16);
    pica_lut_upload(&P->ctx, PICA_LUT_DA(0), lut_buf);

    set_all(P, 256, 0.0f, 0, 0, 1);
    for (int i = 0; i < 256; i++)
        dist_list[i] = 0.25f + 8.0f * i / 255.0f;
    csv_begin(P, "dist_atten", "distance");
    run(P, 256, setup_dist);
    for (int i = 0; i < 256; i++)
        csv_row(P, i, "%.5f", dist_list[i]);
    csv_end(P);
}

/* --- spot: spotlight LUT input ------------------------------------------------ */

static float spot_angle[128];

static void setup_spot(probe* P, int i)
{
    pica_light l;
    basic_light(&l, true, false, false);
    /* Raw register value: unit vector at spot_angle[i] from +Z, as the hardware sees it */
    float a = spot_angle[i];
    int32_t sx = (int32_t)(sinf(a) * 2047.0f), sz = (int32_t)(cosf(a) * 2047.0f);
    l.spot_xy = (uint32_t)sx & 0x1FFF;
    l.spot_z = (uint32_t)sz & 0x1FFF;
    pica_light_bind(&P->ctx, 0, &l);
}

static void exp_spot(probe* P)
{
    pica_light_env env;
    pica_light_env_init(&env);
    env.config0 = PICA_LC0_LAYER(0); /* layer 0 supports SP */
    env.config1 &= ~PICA_LC1_SPOT_OFF(0);
    pica_light_env_lut(&env, PICA_LUTSLOT_SP, PICA_LUTINPUT_SP, false, PICA_LUTSCALE_1X);
    pica_light_env_bind(&P->ctx, &env);
    lut_ramp(16);
    pica_lut_upload(&P->ctx, PICA_LUT_SP(0), lut_buf);

    set_all(P, 128, 0.0f, 0, 0, 1);
    for (int i = 0; i < 128; i++)
        spot_angle[i] = (float)M_PI * i / 127.0f;
    csv_begin(P, "spot", "spot_angle_deg");
    run(P, 128, setup_spot);
    for (int i = 0; i < 128; i++)
        csv_row(P, i, "%.4f", spot_angle[i] * 180.0 / M_PI);
    csv_end(P);
}

/* --- quat_norm: are interpolated/unnormalised quaternions renormalised? ---- */

static void exp_quat_norm(probe* P)
{
    pica_light_env env;
    pica_light l;
    pica_light_env_init(&env);
    pica_light_env_bind(&P->ctx, &env);
    basic_light(&l, true, false, false);
    pica_light_bind(&P->ctx, 0, &l);

    /* same 60-degree rotation, quaternion scaled by 0.25 .. 2 */
    for (int i = 0; i < 64; i++)
    {
        float s = 0.25f + 1.75f * i / 63.0f;
        quat_y(P->quat[i], (float)M_PI / 3.0f);
        for (int k = 0; k < 4; k++)
            P->quat[i][k] *= s;
        P->view[i][0] = 0; P->view[i][1] = 0; P->view[i][2] = 1;
    }
    csv_begin(P, "quat_norm", "quat_scale");
    run(P, 64, setup_none);
    for (int i = 0; i < 64; i++)
        csv_row(P, i, "%.5f", 0.25 + 1.75 * i / 63.0);
    csv_end(P);
}

/* ===========================================================================
 * Main
 * ======================================================================== */

typedef struct
{
    const char* name;
    void (*fn)(probe*);
} experiment;

static const experiment experiments[] = {
    { "diffuse", exp_diffuse_ln },
    { "two_sided", exp_diffuse_two_sided },
    { "lut_value", exp_lut_value },
    { "lut_index", exp_lut_index },
    { "lut_interp", exp_lut_interp },
    { "lut_scale", exp_lut_scale },
    { "layers", exp_layers },
    { "light_bits", exp_light_bits },
    { "dist_atten", exp_dist_atten },
    { "spot", exp_spot },
    { "quat_norm", exp_quat_norm },
};

static void write_meta(void)
{
    FILE* f = fopen(OUT_DIR "/meta.txt", "w");
    if (!f)
        return;
    bool new3ds = false;
    APT_CheckNew3DS(&new3ds);
    u32 fw = osGetFirmVersion(), kv = osGetKernelVersion();
    fprintf(f, "probe_version=%d\n", PROBE_VERSION);
    fprintf(f, "model=%s\n", new3ds ? "new3ds" : "old3ds");
    fprintf(f, "firm=%lu.%lu.%lu\n", GET_VERSION_MAJOR(fw), GET_VERSION_MINOR(fw), GET_VERSION_REVISION(fw));
    fprintf(f, "kernel=%lu.%lu.%lu\n", GET_VERSION_MAJOR(kv), GET_VERSION_MINOR(kv), GET_VERSION_REVISION(kv));
    fprintf(f, "source=%s\n", envIsHomebrew() ? "homebrew" : "title");
    fclose(f);
}

static int probe_init(probe* P)
{
    int res = pica_init(&P->ctx, pica_platform_ctru(), 0);
    if (res)
        return res;
    if ((res = pica_shbin_parse(&P->shbin, lit_shbin, lit_shbin_size)))
        return res;
    if ((res = pica_framebuffer_create(&P->ctx, &P->fb, W, H, PICA_COLOR_RGBA8, PICA_DEPTH_NONE)))
        return res;
    P->vbo = (vertex*)pica_alloc_linear(&P->ctx, sizeof(vertex) * 4 * MAX_SAMPLES);
    P->rb = (uint32_t*)pica_alloc_linear(&P->ctx, W * H * 4);
    if (!P->vbo || !P->rb)
        return PICA_ERR_NOMEM;

    pica_attr_init(&P->attrs);
    pica_attr_add_loader(&P->attrs, 0, PICA_FLOAT, 3);
    pica_attr_add_loader(&P->attrs, 1, PICA_FLOAT, 4);
    pica_attr_add_loader(&P->attrs, 2, PICA_FLOAT, 3);
    pica_buf_init(&P->bufs);
    if (pica_buf_add(&P->ctx, &P->bufs, P->vbo, sizeof(vertex), 3, PICA_PERMUTATION_SEQ(3)) < 0)
        return PICA_ERR_RANGE;
    return PICA_OK;
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    probe* P = &P_;

    say("PICA200 hardware probe v%d\n\n", PROBE_VERSION);
    mkdir(OUT_DIR, 0777);
    write_meta();

    int res = probe_init(P);
    if (res)
        say("init failed: %d\n", res);
    else
    {
        for (size_t i = 0; i < sizeof(experiments) / sizeof(experiments[0]); i++)
            experiments[i].fn(P);
        FILE* f = fopen(OUT_DIR "/done.txt", "w");
        if (f)
        {
            fprintf(f, "failures=%d\n", P->failures);
            fclose(f);
        }
        say("\nFinished (%d errors). Results in " OUT_DIR "\n", P->failures);
    }
    say("Press START to exit.\n");

    for (int frame = 0; frame < 600 && aptMainLoop(); frame++)
    {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        gspWaitForVBlank();
    }
    gfxExit();
    return 0;
}
