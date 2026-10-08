/*
 * Minimal Corgi PICA200 driver example: a spinning vertex-colored triangle
 * in front of a textured, depth-tested floor, rendered to the top screen.
 *
 * On frame 60 the rendered image is also saved to sdmc:/pica_triangle.ppm.
 * Press START to exit.
 */
#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "pica/pica.h"
#include "vshader_shbin.h"

typedef struct
{
    float pos[3];
    float tc[2];
    float color[4];
} vertex;

static const vertex triangle[] = {
    { { -0.8f, -0.6f, 0.0f }, { 0, 0 }, { 1, 0, 0, 1 } },
    { { 0.8f, -0.6f, 0.0f }, { 0, 0 }, { 0, 1, 0, 1 } },
    { { 0.0f, 0.8f, 0.0f }, { 0, 0 }, { 0, 0, 1, 1 } },
};

static const vertex floor_quad[] = {
    { { -3, -1, 1 }, { 0, 0 }, { 1, 1, 1, 1 } },
    { { 3, -1, 1 }, { 6, 0 }, { 1, 1, 1, 1 } },
    { { -3, -1, -5 }, { 0, 6 }, { 1, 1, 1, 1 } },
    { { 3, -1, -5 }, { 6, 6 }, { 1, 1, 1, 1 } },
};

static void save_screenshot(pica_context* ctx, const pica_framebuffer* fb)
{
    uint32_t* px = (uint32_t*)pica_alloc_linear(ctx, fb->width * fb->height * 4);
    if (!px)
        return;
    if (pica_transfer(ctx, fb, px, fb->width, fb->height, PICA_XFER_RGBA8, 0) == PICA_OK)
    {
        FILE* f = fopen("sdmc:/pica_triangle.ppm", "wb");
        if (f)
        {
            /* Rotate back to the landscape orientation the user sees */
            fprintf(f, "P6\n%u %u\n255\n", fb->height, fb->width);
            for (unsigned ly = 0; ly < fb->width; ly++)
                for (unsigned lx = 0; lx < fb->height; lx++)
                {
                    uint32_t c = px[lx * fb->width + (fb->width - 1 - ly)];
                    uint8_t rgb[3] = { (uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8) };
                    fwrite(rgb, 1, 3, f);
                }
            fclose(f);
        }
    }
    pica_free_linear(ctx, px);
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);

    pica_context ctx;
    int res = pica_init(&ctx, pica_platform_ctru(), 0);
    if (res)
    {
        printf("pica_init failed: %d\n", res);
        goto wait_exit;
    }

    /* Shader */
    pica_shbin shbin;
    if ((res = pica_shbin_parse(&shbin, vshader_shbin, vshader_shbin_size)))
    {
        printf("shader parse failed: %d\n", res);
        goto wait_exit;
    }
    const pica_dvle* vsh = &shbin.dvle[0];
    int u_proj = pica_shader_uniform_reg(vsh, "projection");

    /* Render target matching the top screen (portrait: 240x400) */
    pica_framebuffer fb;
    if ((res = pica_framebuffer_create(&ctx, &fb, 240, 400, PICA_COLOR_RGBA8, PICA_DEPTH24_STENCIL8)))
    {
        printf("framebuffer: %d\n", res);
        goto wait_exit;
    }

    /* Geometry in linear memory */
    vertex* vbo = (vertex*)pica_alloc_linear(&ctx, sizeof(triangle) + sizeof(floor_quad));
    memcpy(vbo, floor_quad, sizeof(floor_quad));
    memcpy(vbo + 4, triangle, sizeof(triangle));

    pica_attr_info attrs;
    pica_attr_init(&attrs);
    pica_attr_add_loader(&attrs, 0, PICA_FLOAT, 3);
    pica_attr_add_loader(&attrs, 1, PICA_FLOAT, 2);
    pica_attr_add_loader(&attrs, 2, PICA_FLOAT, 4);
    pica_buf_info bufs;
    pica_buf_init(&bufs);
    pica_buf_add(&ctx, &bufs, vbo, sizeof(vertex), 3, PICA_PERMUTATION_SEQ(3));

    /* 32x32 checkerboard texture */
    static uint32_t img[32 * 32];
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
            img[y * 32 + x] = (((x >> 3) ^ (y >> 3)) & 1) ? 0xE0E0E0FF : 0x406080FF;
    pica_texture tex;
    pica_tex_create(&ctx, &tex, 32, 32, PICA_TEX_RGBA8, 0, false);
    pica_tex_upload(&ctx, &tex, 0, img);
    pica_tex_filter(&tex, PICA_LINEAR, PICA_LINEAR);
    pica_tex_wrap(&tex, PICA_REPEAT, PICA_REPEAT);

    pica_texenv env_tex, env_color;
    pica_texenv_init(&env_tex);
    pica_texenv_src(&env_tex, PICA_TEV_BOTH, PICA_SRC_TEXTURE0, PICA_SRC_PRIMARY_COLOR, 0);
    pica_texenv_func(&env_tex, PICA_TEV_BOTH, PICA_MODULATE);
    pica_texenv_init(&env_color);
    pica_texenv_src(&env_color, PICA_TEV_BOTH, PICA_SRC_PRIMARY_COLOR, 0, 0);

    float proj[16];
    pica_mtx_persp_tilt(proj, 1.0f, 400.0f / 240.0f, 0.1f, 20.0f);

    printf("Corgi PICA200 driver example\nPress START to exit\n");

    for (unsigned frame = 0; aptMainLoop(); frame++)
    {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;

        pica_bind_framebuffer(&ctx, &fb);
        pica_clear(&ctx, &fb, PICA_CLEAR_ALL, 0x203040FF, 0);
        pica_depth_test(&ctx, true, PICA_GREATER, PICA_WRITE_ALL);
        pica_bind_shader(&ctx, vsh, NULL, 0);
        pica_bind_attrs(&ctx, &attrs);
        pica_bind_buffers(&ctx, &bufs);

        /* Floor */
        float mvp[16], model[16];
        pica_mtx_identity(model);
        pica_mtx_translate(model, 0, 0, -1.5f);
        pica_mtx_multiply(mvp, proj, model);
        pica_uniform_mtx4(&ctx, PICA_VERTEX_SHADER, u_proj, mvp);
        pica_tex_bind(&ctx, 0, &tex);
        pica_set_texenv(&ctx, 0, &env_tex);
        pica_draw_arrays(&ctx, PICA_TRIANGLE_STRIP, 0, 4);

        /* Spinning triangle */
        pica_mtx_identity(model);
        pica_mtx_translate(model, 0, 0, -2.5f);
        pica_mtx_rotate_axis(model, 1, frame * 0.03f);
        pica_mtx_multiply(mvp, proj, model);
        pica_uniform_mtx4(&ctx, PICA_VERTEX_SHADER, u_proj, mvp);
        pica_set_texenv(&ctx, 0, &env_color);
        pica_draw_arrays(&ctx, PICA_TRIANGLES, 4, 3);

        if (frame == 60)
            save_screenshot(&ctx, &fb);

        /* Present: untile + convert into the LCD framebuffer */
        u8* screen = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
        pica_transfer(&ctx, &fb, screen, 240, 400, PICA_XFER_RGB8, 0);
        gfxSwapBuffers();
        gspWaitForVBlank();

        int err = pica_get_error(&ctx);
        if (err)
            printf("driver error %d at frame %u\n", err, frame);
    }

    pica_tex_destroy(&ctx, &tex);
    pica_free_linear(&ctx, vbo);
    pica_framebuffer_destroy(&ctx, &fb);
    pica_fini(&ctx);
    gfxExit();
    return 0;

wait_exit:
    while (aptMainLoop())
    {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        gspWaitForVBlank();
    }
    gfxExit();
    return 0;
}
