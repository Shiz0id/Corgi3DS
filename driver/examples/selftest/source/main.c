/*
 * PICA200 driver self-test.
 *
 * Runs the driver test suite on the real GPU (through gsp::Gpu), prints the
 * results on the bottom screen and writes them to the SD card:
 *
 *   sdmc:/pica_selftest/results.txt
 *   sdmc:/pica_selftest/<test>.ppm     (64x64 image of each test's result)
 *
 * Afterwards it shows the last test image, scaled up, on the top screen.
 * Press START to exit (it also exits by itself after ~10 seconds).
 */
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "pica/pica.h"
#include "suite.h"

#define OUT_DIR "sdmc:/pica_selftest"

static FILE* results;

static void log_msg(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (results)
    {
        va_start(ap, fmt);
        vfprintf(results, fmt, ap);
        va_end(ap);
        fflush(results);
    }
}

static uint32_t last_image[64 * 64];

static void dump(const char* name, const uint32_t* px, unsigned w, unsigned h)
{
    char path[128];
    snprintf(path, sizeof(path), OUT_DIR "/%s.ppm", name);
    FILE* f = fopen(path, "wb");
    if (f)
    {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (unsigned i = 0; i < w * h; i++)
        {
            uint8_t rgb[3] = { (uint8_t)(px[i] >> 24), (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8) };
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
    }
    if (w * h <= 64 * 64)
        memcpy(last_image, px, w * h * 4);
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);

    mkdir(OUT_DIR, 0777);
    results = fopen(OUT_DIR "/results.txt", "w");

    log_msg("Corgi PICA200 driver self-test\n\n");
    suite_io io = { log_msg, dump, NULL };
    int failures = suite_run(pica_platform_ctru(), &io);
    log_msg(failures == 0 ? "\nALL TESTS PASSED\n" : "\nFAILURES: %d\n", failures);
    log_msg("Results written to " OUT_DIR "\n");
    if (results)
    {
        fprintf(results, "DONE\n");
        fclose(results);
        results = NULL;
    }

    /* Show the last test image on the top screen, scaled 3x (the LCD
     * framebuffer is BGR8, stored column-major from the bottom-left). */
    for (int frame = 0; frame < 600 && aptMainLoop(); frame++)
    {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        u16 fbw, fbh;
        u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbw, &fbh);
        memset(fb, 0x20, fbw * fbh * 3);
        for (int y = 0; y < 64 * 3; y++)
            for (int x = 0; x < 64 * 3; x++)
            {
                uint32_t c = last_image[(y / 3) * 64 + x / 3];
                int sx = 104 + x, sy = 24 + y; /* landscape coordinates */
                u8* p = fb + ((sx * fbw) + (fbw - 1 - sy)) * 3;
                p[0] = (u8)(c >> 8);
                p[1] = (u8)(c >> 16);
                p[2] = (u8)(c >> 24);
            }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    gfxExit();
    return 0;
}
