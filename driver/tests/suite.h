/*
 * suite.h - rendering test suite shared by the 3DS self-test app and the
 * Corgi3DS host harness.
 */
#ifndef PICA_SUITE_H
#define PICA_SUITE_H

#include "pica/pica.h"

typedef struct suite_io
{
    void (*log)(const char* fmt, ...);
    /* Optional: called with each test's final image (0xRRGGBBAA, row 0 =
     * first row of the render target in memory). */
    void (*dump)(const char* name, const uint32_t* pixels, unsigned w, unsigned h);
    /* Optional: only run tests whose name contains this string. */
    const char* filter;
} suite_io;

/* Initialises a driver context on `plat`, runs every test and returns the
 * number of failed tests (or a negative driver error). */
int suite_run(const pica_platform* plat, const suite_io* io);

#endif
