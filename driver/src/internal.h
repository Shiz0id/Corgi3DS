/*
 * internal.h - shared helpers for the driver implementation.
 */
#ifndef PICA_INTERNAL_H
#define PICA_INTERNAL_H

#include "pica/pica.h"

/* Words kept free at the end of the command buffer for the finalize/flush
 * sequence appended by pica_flush(). */
#define PICA_CMDBUF_TAIL_WORDS 16

/* Record an error (the first one sticks). */
static inline void pica__set_error(pica_context* ctx, int err)
{
    if (!ctx->error)
        ctx->error = err;
}

/* Make room for `words` more command words, flushing to the GPU if needed.
 * Returns false (and records an error) if that is impossible. */
bool pica__reserve(pica_context* ctx, uint32_t words);

/* Low-level writers.  The caller must have reserved space. */
void pica__cmd(pica_context* ctx, uint16_t reg, uint32_t mask, const uint32_t* params, unsigned count,
               bool incremental);

static inline void pica__write(pica_context* ctx, uint16_t reg, uint32_t value)
{
    pica__cmd(ctx, reg, 0xF, &value, 1, false);
}

static inline void pica__write_masked(pica_context* ctx, uint16_t reg, uint32_t mask, uint32_t value)
{
    pica__cmd(ctx, reg, mask, &value, 1, false);
}

static inline void pica__write_inc(pica_context* ctx, uint16_t reg, const uint32_t* values, unsigned count)
{
    pica__cmd(ctx, reg, 0xF, values, count, true);
}

static inline void pica__write_rep(pica_context* ctx, uint16_t reg, const uint32_t* values, unsigned count)
{
    pica__cmd(ctx, reg, 0xF, values, count, false);
}

/* Worst-case words taken by a command carrying `count` parameters. */
static inline uint32_t pica__cmd_words(unsigned count)
{
    uint32_t chunks = (count + PICA_CMD_MAX_PARAMS - 1) / PICA_CMD_MAX_PARAMS;
    if (!chunks)
        chunks = 1;
    return count + chunks * 2;
}

/* 0xRRGGBBAA -> 0xAABBGGRR, the byte order color registers expect. */
static inline uint32_t pica__rgba_to_reg(uint32_t rgba)
{
    return ((rgba >> 24) & 0xFF) | ((rgba >> 8) & 0xFF00) | ((rgba << 8) & 0xFF0000) | ((rgba & 0xFF) << 24);
}

/* Emitted by framebuffer and fragment-state code */
void pica__emit_fragment_state(pica_context* ctx);

#endif /* PICA_INTERNAL_H */
