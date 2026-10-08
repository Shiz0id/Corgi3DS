/*
 * suite.c - rendering tests for the PICA200 driver.
 *
 * Each test renders into a 64x64 RGBA8 + D24S8 target, reads it back with
 * the display transfer engine and checks pixels.  Coordinates in checks are
 * OpenGL-style window coordinates: (0, 0) is the bottom-left pixel.  The
 * PICA200 stores render targets (like textures) top row first, so window
 * row y lives in memory row H-1-y.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "suite.h"
#include "shaders/shaders.h"

#define W 64
#define H 64

typedef struct
{
    float pos[3];
    float color[4];
} cvtx;

typedef struct
{
    float pos[3];
    float tc[2];
    float color[4];
} tvtx;

typedef struct
{
    pica_context ctx;
    const suite_io* io;
    pica_framebuffer fb;
    uint32_t* rb;          /* readback buffer, linear memory */
    pica_shbin sh_color, sh_tex, sh_gsh;
    int proj_color, proj_tex;
    float ident[16];
    cvtx* cbuf;            /* scratch vertex memory */
    tvtx* tbuf;
    uint8_t* scratch;      /* scratch linear memory for indices/textures */
    int failures;
    int checks_failed;     /* in the current test */
    const char* test;
} suite;

/* ------------------------------------------------------------------------- */

#define LOG(...) S->io->log(__VA_ARGS__)

static void fail(suite* S, const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (S->checks_failed < 8)
        LOG("    FAIL %s: %s\n", S->test, buf);
    S->checks_failed++;
}

static void readback(suite* S)
{
    int res = pica_transfer(&S->ctx, &S->fb, S->rb, W, H, PICA_XFER_RGBA8, 0);
    if (res)
        fail(S, "transfer failed (%d)", res);
}

/* Pixel at window coordinates (y = 0 is the bottom row) */
static uint32_t px(suite* S, int x, int y)
{
    return S->rb[(H - 1 - y) * W + x];
}

static int chan(uint32_t c, int i)
{
    return (int)((c >> (24 - 8 * i)) & 0xFF);
}

static bool close_to(uint32_t a, uint32_t b, int tol, bool check_alpha)
{
    for (int i = 0; i < (check_alpha ? 4 : 3); i++)
        if (abs(chan(a, i) - chan(b, i)) > tol)
            return false;
    return true;
}

static void expect_px(suite* S, int x, int y, uint32_t want, int tol, const char* what)
{
    uint32_t got = px(S, x, y);
    if (!close_to(got, want, tol, false))
        fail(S, "%s: pixel (%d,%d) = %08X, expected %08X (+-%d)", what, x, y, (unsigned)got, (unsigned)want, tol);
}

/* Count pixels in a window-space rectangle [x0,x1) x [y0,y1) matching `want` */
static int count_rect(suite* S, int x0, int y0, int x1, int y1, uint32_t want, int tol)
{
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            n += close_to(px(S, x, y), want, tol, false);
    return n;
}

static void expect_rect(suite* S, int x0, int y0, int x1, int y1, uint32_t want, int tol, const char* what)
{
    int n = count_rect(S, x0, y0, x1, y1, want, tol);
    int total = (x1 - x0) * (y1 - y0);
    if (n != total)
    {
        /* report the first offender */
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++)
                if (!close_to(px(S, x, y), want, tol, false))
                {
                    fail(S, "%s: %d/%d pixels in [%d,%d)x[%d,%d) match %08X; first bad (%d,%d) = %08X", what, n,
                         total, x0, x1, y0, y1, (unsigned)want, x, y, (unsigned)px(S, x, y));
                    return;
                }
    }
}

/* ---- Scene helpers ---------------------------------------------------------- */

static void bind_color_program(suite* S)
{
    pica_attr_info ai;
    pica_buf_info bi;

    pica_bind_shader(&S->ctx, &S->sh_color.dvle[0], NULL, 0);
    pica_uniform_mtx4(&S->ctx, PICA_VERTEX_SHADER, S->proj_color, S->ident);

    pica_attr_init(&ai);
    pica_attr_add_loader(&ai, 0, PICA_FLOAT, 3);
    pica_attr_add_loader(&ai, 1, PICA_FLOAT, 4);
    pica_bind_attrs(&S->ctx, &ai);

    pica_buf_init(&bi);
    pica_buf_add(&S->ctx, &bi, S->cbuf, sizeof(cvtx), 2, PICA_PERMUTATION_SEQ(2));
    pica_bind_buffers(&S->ctx, &bi);
}

static void bind_tex_program(suite* S)
{
    pica_attr_info ai;
    pica_buf_info bi;

    pica_bind_shader(&S->ctx, &S->sh_tex.dvle[0], NULL, 0);
    pica_uniform_mtx4(&S->ctx, PICA_VERTEX_SHADER, S->proj_tex, S->ident);

    pica_attr_init(&ai);
    pica_attr_add_loader(&ai, 0, PICA_FLOAT, 3);
    pica_attr_add_loader(&ai, 1, PICA_FLOAT, 2);
    pica_attr_add_loader(&ai, 2, PICA_FLOAT, 4);
    pica_bind_attrs(&S->ctx, &ai);

    pica_buf_init(&bi);
    pica_buf_add(&S->ctx, &bi, S->tbuf, sizeof(tvtx), 3, PICA_PERMUTATION_SEQ(3));
    pica_bind_buffers(&S->ctx, &bi);
}

static void texenv_primary(suite* S)
{
    pica_texenv env;
    for (int i = 0; i < 6; i++)
    {
        pica_texenv_init(&env);
        if (i == 0)
            pica_texenv_src(&env, PICA_TEV_BOTH, PICA_SRC_PRIMARY_COLOR, 0, 0);
        pica_set_texenv(&S->ctx, i, &env);
    }
    pica_texenv_buffer(&S->ctx, 0, 0, 0);
}

static void texenv_texture(suite* S)
{
    pica_texenv env;
    pica_texenv_init(&env);
    pica_texenv_src(&env, PICA_TEV_BOTH, PICA_SRC_TEXTURE0, 0, 0);
    pica_set_texenv(&S->ctx, 0, &env);
}

/* Reset to a known state: opaque writes, no tests, color program bound. */
static void reset(suite* S, uint32_t clear)
{
    pica_context* ctx = &S->ctx;
    pica_bind_framebuffer(ctx, &S->fb);
    pica_cull(ctx, PICA_CULL_NONE);
    pica_scissor(ctx, PICA_SCISSOR_DISABLE, 0, 0, 0, 0);
    pica_depth_map(ctx, true, -1.0f, 0.0f);
    pica_depth_test(ctx, false, PICA_ALWAYS, PICA_WRITE_ALL);
    pica_alpha_test(ctx, false, PICA_ALWAYS, 0);
    pica_stencil_test(ctx, false, PICA_ALWAYS, 0, 0xFF, 0);
    pica_stencil_op(ctx, PICA_STENCIL_KEEP, PICA_STENCIL_KEEP, PICA_STENCIL_KEEP);
    pica_blend(ctx, PICA_BLEND_ADD, PICA_BLEND_ADD, PICA_ONE, PICA_ZERO, PICA_ONE, PICA_ZERO);
    for (int i = 0; i < 3; i++)
        pica_tex_bind(ctx, i, NULL);
    texenv_primary(S);
    bind_color_program(S);
    pica_clear(ctx, &S->fb, PICA_CLEAR_ALL, clear, 0);
}

static void set_cvtx(cvtx* v, float x, float y, float z, uint32_t rgba)
{
    v->pos[0] = x;
    v->pos[1] = y;
    v->pos[2] = z;
    for (int i = 0; i < 4; i++)
        v->color[i] = chan(rgba, i) / 255.0f;
}

/* Axis-aligned quad as a 4-vertex triangle strip (counter-clockwise) */
static void quad(cvtx* v, float x0, float y0, float x1, float y1, float z, uint32_t rgba)
{
    set_cvtx(&v[0], x0, y0, z, rgba);
    set_cvtx(&v[1], x1, y0, z, rgba);
    set_cvtx(&v[2], x0, y1, z, rgba);
    set_cvtx(&v[3], x1, y1, z, rgba);
}

static void draw_quad(suite* S, int slot, float x0, float y0, float x1, float y1, float z, uint32_t rgba)
{
    quad(&S->cbuf[slot * 4], x0, y0, x1, y1, z, rgba);
    pica_draw_arrays(&S->ctx, PICA_TRIANGLE_STRIP, slot * 4, 4);
}

static void set_tvtx(tvtx* v, float x, float y, float u, float t)
{
    v->pos[0] = x;
    v->pos[1] = y;
    v->pos[2] = -0.5f;
    v->tc[0] = u;
    v->tc[1] = t;
    v->color[0] = v->color[1] = v->color[2] = v->color[3] = 1.0f;
}

static void tex_fullscreen(suite* S)
{
    set_tvtx(&S->tbuf[0], -1, -1, 0, 0);
    set_tvtx(&S->tbuf[1], 1, -1, 1, 0);
    set_tvtx(&S->tbuf[2], -1, 1, 0, 1);
    set_tvtx(&S->tbuf[3], 1, 1, 1, 1);
}

/* ---- Tests ------------------------------------------------------------------ */

static void test_clear(suite* S)
{
    reset(S, 0x336699FF);
    readback(S);
    expect_rect(S, 0, 0, W, H, 0x336699FF, 0, "clear color");
}

static void test_orientation(suite* S)
{
    reset(S, 0x000000FF);
    /* Red quad over the bottom-left quadrant of clip space */
    draw_quad(S, 0, -1, -1, 0, 0, -0.5f, 0xFF0000FF);
    readback(S);
    int bl = count_rect(S, 0, 0, W / 2, H / 2, 0xFF0000FF, 0);
    int br = count_rect(S, W / 2, 0, W, H / 2, 0xFF0000FF, 0);
    int tl = count_rect(S, 0, H / 2, W / 2, H, 0xFF0000FF, 0);
    int tr = count_rect(S, W / 2, H / 2, W, H, 0xFF0000FF, 0);
    if (bl != (W / 2) * (H / 2) || br || tl || tr)
        fail(S, "red pixels per quadrant: bottom-left %d, bottom-right %d, top-left %d, top-right %d "
                "(expected %d in bottom-left only)", bl, br, tl, tr, (W / 2) * (H / 2));
}

static void test_interpolation(suite* S)
{
    reset(S, 0x000000FF);
    set_cvtx(&S->cbuf[0], -1, -1, -0.5f, 0xFF0000FF);
    set_cvtx(&S->cbuf[1], 1, -1, -0.5f, 0x00FF00FF);
    set_cvtx(&S->cbuf[2], -1, 1, -0.5f, 0x0000FFFF);
    set_cvtx(&S->cbuf[3], 1, 1, -0.5f, 0xFFFFFFFF);
    pica_draw_arrays(&S->ctx, PICA_TRIANGLE_STRIP, 0, 4);
    readback(S);
    expect_px(S, 0, 0, 0xFF0000FF, 12, "bottom-left");
    expect_px(S, W - 1, 0, 0x00FF00FF, 12, "bottom-right");
    expect_px(S, 0, H - 1, 0x0000FFFF, 12, "top-left");
    expect_px(S, W - 1, H - 1, 0xFFFFFFFF, 12, "top-right");
    /* Along the bottom edge red fades into green */
    expect_px(S, W / 2, 0, 0x808000FF, 12, "bottom-middle");
}

static void test_triangles(suite* S)
{
    /* Independent triangles and triangle fans */
    reset(S, 0x000000FF);
    set_cvtx(&S->cbuf[0], -1, -1, -0.5f, 0x00FF00FF);
    set_cvtx(&S->cbuf[1], 0, -1, -0.5f, 0x00FF00FF);
    set_cvtx(&S->cbuf[2], -1, 0, -0.5f, 0x00FF00FF);
    pica_draw_arrays(&S->ctx, PICA_TRIANGLES, 0, 3);
    /* fan covering the top-right quadrant */
    set_cvtx(&S->cbuf[4], 0, 0, -0.5f, 0xFFFF00FF);
    set_cvtx(&S->cbuf[5], 1, 0, -0.5f, 0xFFFF00FF);
    set_cvtx(&S->cbuf[6], 1, 1, -0.5f, 0xFFFF00FF);
    set_cvtx(&S->cbuf[7], 0, 1, -0.5f, 0xFFFF00FF);
    pica_draw_arrays(&S->ctx, PICA_TRIANGLE_FAN, 4, 4);
    readback(S);
    expect_px(S, 4, 4, 0x00FF00FF, 0, "triangle inside");
    expect_px(S, 28, 28, 0x000000FF, 0, "triangle outside (above hypotenuse)");
    expect_rect(S, W / 2, H / 2, W, H, 0xFFFF00FF, 0, "fan");
    expect_rect(S, W / 2, 0, W, H / 2, 0x000000FF, 0, "untouched quadrant");
}

static void test_depth(suite* S)
{
    reset(S, 0x000000FF);
    pica_depth_test(&S->ctx, true, PICA_GREATER, PICA_WRITE_ALL);
    /* depth = -z: z=-0.75 is nearer than z=-0.25 */
    draw_quad(S, 0, -1, -1, 1, 1, -0.75f, 0x00FF00FF);
    draw_quad(S, 1, -0.5f, -0.5f, 0.5f, 0.5f, -0.25f, 0xFF0000FF);
    /* A nearer quad drawn last must win on the right half */
    draw_quad(S, 2, 0, -1, 1, 1, -0.9f, 0x0000FFFF);
    readback(S);
    expect_px(S, 24, 32, 0x00FF00FF, 0, "far quad hidden");
    expect_px(S, 48, 32, 0x0000FFFF, 0, "near quad drawn");
    expect_px(S, 8, 8, 0x00FF00FF, 0, "background quad");

    /* Without the test, draw order decides */
    reset(S, 0x000000FF);
    draw_quad(S, 0, -1, -1, 1, 1, -0.75f, 0x00FF00FF);
    draw_quad(S, 1, -0.5f, -0.5f, 0.5f, 0.5f, -0.25f, 0xFF0000FF);
    readback(S);
    expect_px(S, 32, 32, 0xFF0000FF, 0, "depth test disabled");
}

static void test_blend(suite* S)
{
    reset(S, 0x0000FFFF);
    pica_blend(&S->ctx, PICA_BLEND_ADD, PICA_BLEND_ADD, PICA_SRC_ALPHA, PICA_ONE_MINUS_SRC_ALPHA, PICA_ONE,
               PICA_ZERO);
    draw_quad(S, 0, -1, -1, 1, 1, -0.5f, 0xFF000080);
    readback(S);
    expect_px(S, 32, 32, 0x80007FFF, 3, "src-alpha blend");

    reset(S, 0x204060FF);
    pica_blend(&S->ctx, PICA_BLEND_ADD, PICA_BLEND_ADD, PICA_ONE, PICA_ONE, PICA_ONE, PICA_ZERO);
    draw_quad(S, 0, -1, -1, 1, 1, -0.5f, 0x404040FF);
    readback(S);
    expect_px(S, 32, 32, 0x6080A0FF, 3, "additive blend");
}

static void test_alpha_test(suite* S)
{
    reset(S, 0x000000FF);
    pica_alpha_test(&S->ctx, true, PICA_GREATER, 0x80);
    draw_quad(S, 0, -1, -1, 0, 1, -0.5f, 0xFF000040); /* fails: 0x40 <= 0x80 */
    draw_quad(S, 1, 0, -1, 1, 1, -0.5f, 0x00FF00C0);  /* passes */
    readback(S);
    expect_px(S, 16, 32, 0x000000FF, 0, "alpha test reject");
    expect_px(S, 48, 32, 0x00FF00FF, 0, "alpha test pass");
}

static void test_scissor(suite* S)
{
    reset(S, 0x000000FF);
    pica_scissor(&S->ctx, PICA_SCISSOR_NORMAL, 16, 16, 48, 48);
    draw_quad(S, 0, -1, -1, 1, 1, -0.5f, 0xFFFFFFFF);
    readback(S);
    int n = count_rect(S, 0, 0, W, H, 0xFFFFFFFF, 0);
    if (n != 32 * 32)
        fail(S, "%d pixels inside scissor box, expected 1024", n);
    expect_rect(S, 16, 16, 48, 48, 0xFFFFFFFF, 0, "scissor box");
}

static void test_stencil(suite* S)
{
    reset(S, 0x000000FF);
    /* Pass 1: write stencil = 1 in the centre, no color writes */
    pica_depth_test(&S->ctx, false, PICA_ALWAYS, 0);
    pica_stencil_test(&S->ctx, true, PICA_ALWAYS, 1, 0xFF, 0xFF);
    pica_stencil_op(&S->ctx, PICA_STENCIL_KEEP, PICA_STENCIL_KEEP, PICA_STENCIL_REPLACE);
    draw_quad(S, 0, -0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0xFFFFFFFF);
    /* Pass 2: draw red where stencil == 1 */
    pica_depth_test(&S->ctx, false, PICA_ALWAYS, PICA_WRITE_COLOR);
    pica_stencil_test(&S->ctx, true, PICA_EQUAL, 1, 0xFF, 0);
    pica_stencil_op(&S->ctx, PICA_STENCIL_KEEP, PICA_STENCIL_KEEP, PICA_STENCIL_KEEP);
    draw_quad(S, 1, -1, -1, 1, 1, -0.5f, 0xFF0000FF);
    readback(S);
    expect_rect(S, 16, 16, 48, 48, 0xFF0000FF, 0, "stencil pass region");
    expect_px(S, 4, 4, 0x000000FF, 0, "stencil reject region");
    expect_px(S, 60, 60, 0x000000FF, 0, "stencil reject region");
}

static void test_color_mask(suite* S)
{
    reset(S, 0x000000FF);
    pica_depth_test(&S->ctx, false, PICA_ALWAYS, PICA_WRITE_RED | PICA_WRITE_BLUE);
    draw_quad(S, 0, -1, -1, 1, 1, -0.5f, 0xFFFFFFFF);
    readback(S);
    expect_px(S, 32, 32, 0xFF00FFFF, 0, "red+blue write mask");
}

static void test_cull(suite* S)
{
    reset(S, 0x000000FF);
    pica_cull(&S->ctx, PICA_CULL_BACK_CCW);
    /* left half: counter-clockwise strip (front facing) */
    draw_quad(S, 0, -1, -1, 0, 1, -0.5f, 0x00FF00FF);
    /* right half: clockwise strip (back facing) */
    cvtx* v = &S->cbuf[4];
    set_cvtx(&v[0], 0, -1, -0.5f, 0xFF0000FF);
    set_cvtx(&v[1], 0, 1, -0.5f, 0xFF0000FF);
    set_cvtx(&v[2], 1, -1, -0.5f, 0xFF0000FF);
    set_cvtx(&v[3], 1, 1, -0.5f, 0xFF0000FF);
    pica_draw_arrays(&S->ctx, PICA_TRIANGLE_STRIP, 4, 4);
    readback(S);
    expect_px(S, 16, 32, 0x00FF00FF, 0, "front face kept");
    expect_px(S, 48, 32, 0x000000FF, 0, "back face culled");
}

static void test_elements(suite* S)
{
    reset(S, 0x000000FF);
    quad(&S->cbuf[0], -1, -1, 0, 1, -0.5f, 0x00FFFFFF);
    quad(&S->cbuf[4], 0, -1, 1, 1, -0.5f, 0xFF00FFFF);
    uint8_t* i8 = S->scratch;
    uint16_t* i16 = (uint16_t*)(S->scratch + 64);
    static const uint8_t idx[6] = { 0, 1, 2, 2, 1, 3 };
    for (int i = 0; i < 6; i++)
    {
        i8[i] = idx[i];
        i16[i] = (uint16_t)(idx[i] + 4);
    }
    pica_flush_dcache(&S->ctx, S->scratch, 128);
    pica_draw_elements(&S->ctx, PICA_TRIANGLES, 6, 0, i8);
    pica_draw_elements(&S->ctx, PICA_TRIANGLES, 6, 1, i16);
    readback(S);
    expect_rect(S, 0, 0, W / 2, H, 0x00FFFFFF, 0, "u8 indexed quad");
    expect_rect(S, W / 2, 0, W, H, 0xFF00FFFF, 0, "u16 indexed quad");
}

static void test_immediate(suite* S)
{
    reset(S, 0x000000FF);
    pica_imm_begin(&S->ctx, PICA_TRIANGLE_STRIP);
    const float pts[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
    for (int i = 0; i < 4; i++)
    {
        pica_imm_attr(&S->ctx, pts[i][0], pts[i][1], -0.5f, 1.0f);
        pica_imm_attr(&S->ctx, 1.0f, 0.5f, 0.0f, 1.0f);
    }
    pica_imm_end(&S->ctx);
    readback(S);
    expect_rect(S, 0, 0, W, H, 0xFF8000FF, 2, "immediate-mode quad");
}

static void test_fixed_attrib(suite* S)
{
    pica_attr_info ai;
    pica_buf_info bi;

    reset(S, 0x000000FF);
    /* Position from the buffer, color from a fixed attribute */
    pica_attr_init(&ai);
    pica_attr_add_loader(&ai, 0, PICA_FLOAT, 3);
    pica_attr_add_fixed(&ai, 1);
    pica_bind_attrs(&S->ctx, &ai);
    pica_buf_init(&bi);
    /* each vertex is still a cvtx; skip the color with 16 bytes of padding */
    pica_buf_add(&S->ctx, &bi, S->cbuf, sizeof(cvtx), 2, 0xF0);
    pica_bind_buffers(&S->ctx, &bi);
    pica_fixed_attrib(&S->ctx, 1, 1.0f, 1.0f, 0.0f, 1.0f);
    draw_quad(S, 0, -1, -1, 1, 1, -0.5f, 0x0000FFFF);
    readback(S);
    expect_rect(S, 0, 0, W, H, 0xFFFF00FF, 1, "fixed attribute color");
}

static void test_logic_op(suite* S)
{
    reset(S, 0x336699FF);
    pica_logic_op(&S->ctx, PICA_LOGICOP_XOR);
    draw_quad(S, 0, -1, -1, 1, 1, -0.5f, 0xFF0F00FF);
    readback(S);
    expect_px(S, 32, 32, 0xCC6999FF, 0, "xor logic op");
}

static void test_texture(suite* S)
{
    pica_texture tex;
    uint32_t img[8 * 8];

    /* Quadrants: top-left red, top-right green, bottom-left blue, bottom-right white
     * (row 0 = top of the image). */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            img[y * 8 + x] = y < 4 ? (x < 4 ? 0xFF0000FF : 0x00FF00FF) : (x < 4 ? 0x0000FFFF : 0xFFFFFFFF);

    reset(S, 0x000000FF);
    if (pica_tex_create(&S->ctx, &tex, 8, 8, PICA_TEX_RGBA8, 0, false) || pica_tex_upload(&S->ctx, &tex, 0, img))
    {
        fail(S, "texture creation failed");
        return;
    }
    bind_tex_program(S);
    texenv_texture(S);
    pica_tex_bind(&S->ctx, 0, &tex);
    tex_fullscreen(S);
    pica_draw_arrays(&S->ctx, PICA_TRIANGLE_STRIP, 0, 4);
    readback(S);
    expect_rect(S, 2, 34, 30, 62, 0xFF0000FF, 0, "texture top-left");
    expect_rect(S, 34, 34, 62, 62, 0x00FF00FF, 0, "texture top-right");
    expect_rect(S, 2, 2, 30, 30, 0x0000FFFF, 0, "texture bottom-left");
    expect_rect(S, 34, 2, 62, 30, 0xFFFFFFFF, 0, "texture bottom-right");
    pica_tex_bind(&S->ctx, 0, NULL);
    pica_tex_destroy(&S->ctx, &tex);
}

static void test_texture_wrap(suite* S)
{
    pica_texture tex;
    uint32_t img[8 * 8];

    /* Left half red, right half green */
    for (int i = 0; i < 64; i++)
        img[i] = (i % 8) < 4 ? 0xFF0000FF : 0x00FF00FF;

    reset(S, 0x000000FF);
    if (pica_tex_create(&S->ctx, &tex, 8, 8, PICA_TEX_RGBA8, 0, false) || pica_tex_upload(&S->ctx, &tex, 0, img))
    {
        fail(S, "texture creation failed");
        return;
    }
    pica_tex_wrap(&tex, PICA_REPEAT, PICA_REPEAT);
    bind_tex_program(S);
    texenv_texture(S);
    pica_tex_bind(&S->ctx, 0, &tex);
    /* u from 0 to 2: the pattern repeats twice across the screen */
    set_tvtx(&S->tbuf[0], -1, -1, 0, 0);
    set_tvtx(&S->tbuf[1], 1, -1, 2, 0);
    set_tvtx(&S->tbuf[2], -1, 1, 0, 1);
    set_tvtx(&S->tbuf[3], 1, 1, 2, 1);
    pica_draw_arrays(&S->ctx, PICA_TRIANGLE_STRIP, 0, 4);
    readback(S);
    expect_px(S, 8, 32, 0xFF0000FF, 0, "repeat, 1st period left");
    expect_px(S, 24, 32, 0x00FF00FF, 0, "repeat, 1st period right");
    expect_px(S, 40, 32, 0xFF0000FF, 0, "repeat, 2nd period left");
    expect_px(S, 56, 32, 0x00FF00FF, 0, "repeat, 2nd period right");

    /* Clamp to edge: everything past u=1 takes the rightmost column */
    pica_tex_wrap(&tex, PICA_CLAMP_TO_EDGE, PICA_CLAMP_TO_EDGE);
    pica_tex_bind(&S->ctx, 0, &tex);
    pica_draw_arrays(&S->ctx, PICA_TRIANGLE_STRIP, 0, 4);
    readback(S);
    expect_px(S, 8, 32, 0xFF0000FF, 0, "clamp, inside");
    expect_px(S, 48, 32, 0x00FF00FF, 0, "clamp, past edge");
    expect_px(S, 60, 32, 0x00FF00FF, 0, "clamp, past edge");
    pica_tex_bind(&S->ctx, 0, NULL);
    pica_tex_destroy(&S->ctx, &tex);
}

/* Render an 8x8 solid texture of format `fmt` built from one native texel
 * and compare the result. */
static void tex_format_case(suite* S, const char* name, pica_tex_format_t fmt, const uint8_t* texel,
                            unsigned texel_bits, uint32_t want, uint32_t const_color)
{
    pica_texture tex;
    uint8_t linear[8 * 8 * 4];
    pica_texenv env;

    if (texel_bits >= 8)
    {
        unsigned bytes = texel_bits / 8;
        for (int i = 0; i < 64; i++)
            memcpy(&linear[i * bytes], texel, bytes);
    }
    else
        memset(linear, texel[0] | (texel[0] << 4), 32);

    reset(S, const_color);
    if (pica_tex_create(&S->ctx, &tex, 8, 8, fmt, 0, false) || pica_tex_upload(&S->ctx, &tex, 0, linear))
    {
        fail(S, "%s: texture creation failed", name);
        return;
    }
    bind_tex_program(S);
    /* Output texture RGB and alpha as grey so both are visible:
     * stage 0 = texture rgb; stage 1 = texture alpha into the blue channel would
     * need dot products, so instead we test alpha via blending against black. */
    pica_texenv_init(&env);
    pica_texenv_src(&env, PICA_TEV_RGB, PICA_SRC_TEXTURE0, 0, 0);
    pica_texenv_src(&env, PICA_TEV_ALPHA, PICA_SRC_TEXTURE0, 0, 0);
    pica_set_texenv(&S->ctx, 0, &env);
    pica_tex_bind(&S->ctx, 0, &tex);
    tex_fullscreen(S);
    pica_draw_arrays(&S->ctx, PICA_TRIANGLE_STRIP, 0, 4);
    readback(S);
    uint32_t got = px(S, 32, 32);
    if (!close_to(got, want, 1, true))
        fail(S, "%s: got %08X, expected %08X", name, (unsigned)got, (unsigned)want);
    pica_tex_bind(&S->ctx, 0, NULL);
    pica_tex_destroy(&S->ctx, &tex);
}

static void test_texture_formats(suite* S)
{
    /* Each texel is encoded in the format's native byte order. */
    const uint8_t rgba8[4] = { 0x44, 0x33, 0x22, 0x11 };   /* 0x11223344 -> r=11 g=22 b=33 a=44 */
    const uint8_t rgb8[3] = { 0x30, 0x20, 0x10 };          /* bytes b, g, r */
    const uint16_t rgb565 = (0x1F << 11) | (0x20 << 5) | 0x00;           /* r=FF g=82 b=00 */
    const uint16_t rgba5551 = (0x00 << 11) | (0x1F << 6) | (0x10 << 1) | 1; /* r=00 g=FF b=84 a=FF */
    const uint16_t rgba4 = 0x48C3;                         /* r=44 g=88 b=CC a=33 */
    const uint16_t la8 = 0x80FF;                           /* l=80 a=FF */
    const uint8_t l8 = 0x60, a8 = 0x90, la4 = 0xA5;

    tex_format_case(S, "RGBA8", PICA_TEX_RGBA8, rgba8, 32, 0x11223344, 0x000000FF);
    tex_format_case(S, "RGB8", PICA_TEX_RGB8, rgb8, 24, 0x102030FF, 0x000000FF);
    tex_format_case(S, "RGB565", PICA_TEX_RGB565, (const uint8_t*)&rgb565, 16, 0xFF8200FF, 0x000000FF);
    tex_format_case(S, "RGBA5551", PICA_TEX_RGBA5551, (const uint8_t*)&rgba5551, 16, 0x00FF84FF, 0x000000FF);
    tex_format_case(S, "RGBA4", PICA_TEX_RGBA4, (const uint8_t*)&rgba4, 16, 0x4488CC33, 0x000000FF);
    tex_format_case(S, "LA8", PICA_TEX_LA8, (const uint8_t*)&la8, 16, 0x808080FF, 0x000000FF);
    tex_format_case(S, "L8", PICA_TEX_L8, &l8, 8, 0x606060FF, 0x000000FF);
    tex_format_case(S, "A8", PICA_TEX_A8, &a8, 8, 0x00000090, 0x000000FF);
    tex_format_case(S, "LA4", PICA_TEX_LA4, &la4, 8, 0xAAAAAA55, 0x000000FF);
    const uint8_t l4 = 0x9, a4 = 0x6;
    tex_format_case(S, "L4", PICA_TEX_L4, &l4, 4, 0x999999FF, 0x000000FF);
    tex_format_case(S, "A4", PICA_TEX_A4, &a4, 4, 0x00000066, 0x000000FF);
}

static void test_vram_texture(suite* S)
{
    pica_texture tex;
    uint32_t img[16 * 16];

    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            img[y * 16 + x] = ((x ^ y) & 1) ? 0xFFFFFFFF : 0x000000FF;

    reset(S, 0x000000FF);
    int res = pica_tex_create(&S->ctx, &tex, 16, 16, PICA_TEX_RGBA8, 0, true);
    if (!res)
        res = pica_tex_upload(&S->ctx, &tex, 0, img);
    if (res)
    {
        fail(S, "VRAM texture creation/upload failed (%d)", res);
        return;
    }
    bind_tex_program(S);
    texenv_texture(S);
    pica_tex_bind(&S->ctx, 0, &tex);
    tex_fullscreen(S);
    pica_draw_arrays(&S->ctx, PICA_TRIANGLE_STRIP, 0, 4);
    readback(S);
    /* 16x16 checkerboard over 64x64: 4x4 pixel cells; window cell (cx, cy)
     * maps to texel (cx, 15 - cy) */
    for (int cy = 0; cy < 16; cy += 5)
        for (int cx = 0; cx < 16; cx += 3)
        {
            uint32_t want = ((cx ^ (15 - cy)) & 1) ? 0xFFFFFFFF : 0x000000FF;
            expect_px(S, cx * 4 + 2, cy * 4 + 2, want, 0, "VRAM checkerboard");
        }
    pica_tex_bind(&S->ctx, 0, NULL);
    pica_tex_destroy(&S->ctx, &tex);
}

static void test_combiners(suite* S)
{
    pica_texenv env;

    /* stage 0: primary color * constant; stage 1: previous + constant */
    reset(S, 0x000000FF);
    pica_texenv_init(&env);
    pica_texenv_src(&env, PICA_TEV_BOTH, PICA_SRC_PRIMARY_COLOR, PICA_SRC_CONSTANT, 0);
    pica_texenv_func(&env, PICA_TEV_BOTH, PICA_MODULATE);
    pica_texenv_color(&env, 0x80FF40FF);
    pica_set_texenv(&S->ctx, 0, &env);
    pica_texenv_init(&env);
    pica_texenv_src(&env, PICA_TEV_BOTH, PICA_SRC_PREVIOUS, PICA_SRC_CONSTANT, 0);
    pica_texenv_func(&env, PICA_TEV_BOTH, PICA_ADD);
    pica_texenv_color(&env, 0x10101000);
    pica_set_texenv(&S->ctx, 1, &env);
    draw_quad(S, 0, -1, -1, 1, 1, -0.5f, 0xFF8080FF);
    readback(S);
    /* (FF,80,80) * (80,FF,40) / 255 = (80,80,20) ; + 10 = (90,90,30) */
    expect_px(S, 32, 32, 0x909030FF, 2, "modulate + add");

    /* Combiner buffer: stage 0 = constant red, kept in the buffer;
     * stage 1 = constant green; stage 2 = previous buffer (red again) */
    reset(S, 0x000000FF);
    pica_texenv_init(&env);
    pica_texenv_src(&env, PICA_TEV_BOTH, PICA_SRC_CONSTANT, 0, 0);
    pica_texenv_color(&env, 0xFF0000FF);
    pica_set_texenv(&S->ctx, 0, &env);
    pica_texenv_color(&env, 0x00FF00FF);
    pica_set_texenv(&S->ctx, 1, &env);
    pica_texenv_init(&env);
    pica_texenv_src(&env, PICA_TEV_BOTH, PICA_SRC_PREVIOUS_BUFFER, 0, 0);
    pica_set_texenv(&S->ctx, 2, &env);
    /* The buffer is written by stage n for the input of stage n+1: update
     * after stage 0 only. */
    pica_texenv_buffer(&S->ctx, 0x1, 0x1, 0x000000FF);
    draw_quad(S, 0, -1, -1, 1, 1, -0.5f, 0xFFFFFFFF);
    readback(S);
    expect_px(S, 32, 32, 0xFF0000FF, 0, "previous buffer");
    pica_texenv_buffer(&S->ctx, 0, 0, 0);

    /* Interpolate: lerp(constant blue, primary yellow, 0.25 from texenv operand) */
    reset(S, 0x000000FF);
    pica_texenv_init(&env);
    pica_texenv_src(&env, PICA_TEV_RGB, PICA_SRC_PRIMARY_COLOR, PICA_SRC_CONSTANT, PICA_SRC_CONSTANT);
    pica_texenv_op_rgb(&env, PICA_OP_RGB_SRC_COLOR, PICA_OP_RGB_SRC_COLOR, PICA_OP_RGB_SRC_ALPHA);
    pica_texenv_func(&env, PICA_TEV_RGB, PICA_INTERPOLATE);
    pica_texenv_src(&env, PICA_TEV_ALPHA, PICA_SRC_PRIMARY_COLOR, 0, 0);
    pica_texenv_color(&env, 0x0000FF40); /* blue, alpha = 0.25 */
    pica_set_texenv(&S->ctx, 0, &env);
    draw_quad(S, 0, -1, -1, 1, 1, -0.5f, 0xFFFF00FF);
    readback(S);
    /* s0*s2 + s1*(1-s2) = yellow*0.25 + blue*0.75 = (40,40,BF) */
    expect_px(S, 32, 32, 0x4040BFFF, 2, "interpolate");
}

static void test_geometry_shader(suite* S)
{
    pica_attr_info ai;
    pica_buf_info bi;

    reset(S, 0x000000FF);
    int size_reg = pica_shader_uniform_reg(&S->sh_gsh.dvle[1], "size");
    if (size_reg < 0)
    {
        fail(S, "uniform 'size' not found in geometry shader");
        return;
    }
    pica_bind_shader(&S->ctx, &S->sh_gsh.dvle[0], &S->sh_gsh.dvle[1], 2);
    pica_uniform_4f(&S->ctx, PICA_GEOMETRY_SHADER, size_reg, 0.25f, 0, 0, 0);

    pica_attr_init(&ai);
    pica_attr_add_loader(&ai, 0, PICA_FLOAT, 3);
    pica_attr_add_loader(&ai, 1, PICA_FLOAT, 4);
    pica_bind_attrs(&S->ctx, &ai);
    pica_buf_init(&bi);
    pica_buf_add(&S->ctx, &bi, S->cbuf, sizeof(cvtx), 2, PICA_PERMUTATION_SEQ(2));
    pica_bind_buffers(&S->ctx, &bi);

    /* Two points -> two squares; w must be 1 since the vertex shader passes
     * the position through untouched. */
    set_cvtx(&S->cbuf[0], -0.5f, -0.5f, -0.5f, 0x00FFFFFF);
    set_cvtx(&S->cbuf[1], 0.5f, 0.5f, -0.5f, 0xFF00FFFF);
    pica_draw_arrays(&S->ctx, PICA_GEOMETRY_PRIM, 0, 2);
    readback(S);
    expect_rect(S, 10, 10, 22, 22, 0x00FFFFFF, 0, "gsh square 1");
    expect_rect(S, 42, 42, 54, 54, 0xFF00FFFF, 0, "gsh square 2");
    expect_px(S, 48, 16, 0x000000FF, 0, "gsh outside");
    expect_px(S, 32, 32, 0x000000FF, 0, "gsh outside");

    /* Switching back to a vertex-only program must work again */
    reset(S, 0x000000FF);
    draw_quad(S, 0, -1, -1, 1, 1, -0.5f, 0x123456FF);
    readback(S);
    expect_px(S, 32, 32, 0x123456FF, 0, "vertex shader after geometry shader");
}

static void test_projection(suite* S)
{
    float proj[16], model[16], mvp[16];

    reset(S, 0x000000FF);
    /* Ortho projection mapping a 64x64 "screen" (landscape coords, rotated) */
    pica_mtx_ortho_tilt(proj, 0, 64, 0, 64, 0.1f, 10.0f);
    pica_mtx_identity(model);
    pica_mtx_translate(model, 0, 0, -1.0f);
    pica_mtx_multiply(mvp, proj, model);
    pica_uniform_mtx4(&S->ctx, PICA_VERTEX_SHADER, S->proj_color, mvp);
    /* Landscape rect x 0..16, y 0..64 (left quarter of the landscape image) */
    quad(&S->cbuf[0], 0, 0, 16, 64, 0, 0xFFFFFFFF);
    pica_draw_arrays(&S->ctx, PICA_TRIANGLE_STRIP, 0, 4);
    readback(S);
    /* The tilt maps landscape x to the framebuffer's vertical axis: the
     * landscape left edge ends up at the top of the (portrait) target. */
    int n = count_rect(S, 0, 0, W, H, 0xFFFFFFFF, 0);
    if (n != 16 * 64)
        fail(S, "%d white pixels, expected %d", n, 16 * 64);
    expect_rect(S, 0, 48, 64, 64, 0xFFFFFFFF, 0, "rotated strip");
}

static void test_many_draws(suite* S)
{
    /* Far more commands than fit in the 32 KiB command buffer: exercises
     * automatic splitting into several command lists. */
    reset(S, 0x000000FF);
    const int cells = 16; /* 16x16 grid of 4x4 pixel quads */
    for (int i = 0; i < cells * cells; i++)
    {
        int cx = i % cells, cy = i / cells;
        float x0 = -1.0f + cx * (2.0f / cells), y0 = -1.0f + cy * (2.0f / cells);
        uint32_t c = ((cx + cy) & 1) ? 0xFFFFFFFF : 0x0080FFFF;
        quad(&S->cbuf[i * 4], x0, y0, x0 + 2.0f / cells, y0 + 2.0f / cells, -0.5f, c);
    }
    for (int rep = 0; rep < 4; rep++)
        for (int i = 0; i < cells * cells; i++)
            pica_draw_arrays(&S->ctx, PICA_TRIANGLE_STRIP, i * 4, 4);
    readback(S);
    for (int cy = 0; cy < cells; cy++)
        for (int cx = 0; cx < cells; cx++)
        {
            uint32_t want = ((cx + cy) & 1) ? 0xFFFFFFFF : 0x0080FFFF;
            expect_px(S, cx * 4 + 2, cy * 4 + 2, want, 0, "grid cell");
        }
}

/* ------------------------------------------------------------------------- */

typedef struct
{
    const char* name;
    void (*fn)(suite*);
} test_entry;

static const test_entry tests[] = {
    { "clear", test_clear },
    { "orientation", test_orientation },
    { "interpolation", test_interpolation },
    { "triangles", test_triangles },
    { "depth", test_depth },
    { "blend", test_blend },
    { "alpha_test", test_alpha_test },
    { "scissor", test_scissor },
    { "stencil", test_stencil },
    { "color_mask", test_color_mask },
    { "cull", test_cull },
    { "elements", test_elements },
    { "immediate", test_immediate },
    { "fixed_attrib", test_fixed_attrib },
    { "logic_op", test_logic_op },
    { "texture", test_texture },
    { "texture_wrap", test_texture_wrap },
    { "texture_formats", test_texture_formats },
    { "vram_texture", test_vram_texture },
    { "combiners", test_combiners },
    { "geometry_shader", test_geometry_shader },
    { "projection", test_projection },
    { "many_draws", test_many_draws },
};

int suite_run(const pica_platform* plat, const suite_io* io)
{
    static suite s;
    suite* S = &s;
    int res;

    memset(S, 0, sizeof(*S));
    S->io = io;

    res = pica_init(&S->ctx, plat, 32 * 1024);
    if (res)
    {
        LOG("pica_init failed: %d\n", res);
        return res;
    }

    if ((res = pica_shbin_parse(&S->sh_color, shbin_color, sizeof(shbin_color))) ||
        (res = pica_shbin_parse(&S->sh_tex, shbin_texture, sizeof(shbin_texture))) ||
        (res = pica_shbin_parse(&S->sh_gsh, shbin_gsh_quad, sizeof(shbin_gsh_quad))))
    {
        LOG("shader parse failed: %d\n", res);
        return res;
    }
    S->proj_color = pica_shader_uniform_reg(&S->sh_color.dvle[0], "projection");
    S->proj_tex = pica_shader_uniform_reg(&S->sh_tex.dvle[0], "projection");
    if (S->proj_color < 0 || S->proj_tex < 0)
    {
        LOG("projection uniform not found\n");
        return PICA_ERR_FORMAT;
    }
    pica_mtx_identity(S->ident);

    res = pica_framebuffer_create(&S->ctx, &S->fb, W, H, PICA_COLOR_RGBA8, PICA_DEPTH24_STENCIL8);
    S->rb = (uint32_t*)pica_alloc_linear(&S->ctx, W * H * 4);
    S->cbuf = (cvtx*)pica_alloc_linear(&S->ctx, sizeof(cvtx) * 1024);
    S->tbuf = (tvtx*)pica_alloc_linear(&S->ctx, sizeof(tvtx) * 16);
    S->scratch = (uint8_t*)pica_alloc_linear(&S->ctx, 4096);
    if (res || !S->rb || !S->cbuf || !S->tbuf || !S->scratch)
    {
        LOG("allocation failed\n");
        return PICA_ERR_NOMEM;
    }

    int passed = 0, run = 0;
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
    {
        if (io->filter && !strstr(tests[i].name, io->filter))
            continue;
        S->test = tests[i].name;
        S->checks_failed = 0;
        tests[i].fn(S);
        int err = pica_get_error(&S->ctx);
        if (err)
            fail(S, "driver reported error %d", err);
        run++;
        if (S->checks_failed)
        {
            S->failures++;
            LOG("[FAIL] %s (%d failed checks)\n", tests[i].name, S->checks_failed);
        }
        else
        {
            passed++;
            LOG("[ OK ] %s\n", tests[i].name);
        }
        if (io->dump)
            io->dump(tests[i].name, S->rb, W, H);
    }
    LOG("%d/%d tests passed\n", passed, run);

    pica_free_linear(&S->ctx, S->scratch);
    pica_free_linear(&S->ctx, S->tbuf);
    pica_free_linear(&S->ctx, S->cbuf);
    pica_free_linear(&S->ctx, S->rb);
    pica_framebuffer_destroy(&S->ctx, &S->fb);
    pica_fini(&S->ctx);
    return S->failures;
}
