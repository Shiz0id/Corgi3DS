/*
 * cmdbuf.c - command list encoding and submission.
 */
#include <string.h>
#include "internal.h"

void pica__cmd(pica_context* ctx, uint16_t reg, uint32_t mask, const uint32_t* params, unsigned count,
               bool incremental)
{
    pica_cmdbuf* cb = &ctx->cmd;

    if (!count)
        return;

    while (count)
    {
        unsigned n = count > PICA_CMD_MAX_PARAMS ? PICA_CMD_MAX_PARAMS : count;
        unsigned extra = n - 1;
        uint32_t needed = n + 1 + (extra & 1);

        if (cb->pos + needed > cb->capacity)
        {
            pica__set_error(ctx, PICA_ERR_CMDBUF);
            return;
        }

        cb->buf[cb->pos++] = params[0];
        cb->buf[cb->pos++] = PICA_CMD_HEADER(reg, mask, extra, incremental);
        if (extra)
        {
            memcpy(&cb->buf[cb->pos], &params[1], extra * sizeof(uint32_t));
            cb->pos += extra;
            if (extra & 1)
                cb->buf[cb->pos++] = 0; /* keep 8-byte alignment */
        }

        params += n;
        count -= n;
        if (incremental)
            reg += n;
    }
}

bool pica__reserve(pica_context* ctx, uint32_t words)
{
    pica_cmdbuf* cb = &ctx->cmd;
    uint32_t usable = cb->capacity - PICA_CMDBUF_TAIL_WORDS;

    if (words > usable)
    {
        pica__set_error(ctx, PICA_ERR_CMDBUF);
        return false;
    }
    if (cb->pos + words > usable)
    {
        /* Registers keep their values across command lists, so it is always
         * safe to split here. */
        if (pica_flush(ctx) != PICA_OK)
            return false;
    }
    return true;
}

int pica_flush(pica_context* ctx)
{
    pica_cmdbuf* cb = &ctx->cmd;

    if (!cb->pos)
        return PICA_OK;

    if (ctx->draw_used)
    {
        /* Write back and drop the framebuffer caches so the results are in
         * memory, and so the next list sees any external changes (clears,
         * transfers) to the render target. */
        pica__write(ctx, PICA_REG_FRAMEBUFFER_FLUSH, 1);
        pica__write(ctx, PICA_REG_FRAMEBUFFER_INVALIDATE, 1);
        pica__write(ctx, PICA_REG_EARLYDEPTH_CLEAR, 1);
        ctx->draw_used = false;
    }

    /* Finish with a FINALIZE write (raises the completion IRQ) and pad the
     * list to a multiple of 16 bytes. */
    pica__write(ctx, PICA_REG_FINALIZE, 0x12345678);
    if (cb->pos & 3)
        pica__write(ctx, PICA_REG_FINALIZE, 0x12345678);

    if (ctx->error == PICA_ERR_CMDBUF)
    {
        cb->pos = 0;
        return PICA_ERR_CMDBUF;
    }

    pica_flush_dcache(ctx, cb->buf, cb->pos * sizeof(uint32_t));
    int res = ctx->plat.run_cmdlist(ctx->plat.user, cb->buf, cb->pos * sizeof(uint32_t));
    cb->pos = 0;
    if (res)
    {
        pica__set_error(ctx, PICA_ERR_PLATFORM);
        return PICA_ERR_PLATFORM;
    }
    return PICA_OK;
}

void pica_write_reg(pica_context* ctx, uint16_t reg, uint32_t value)
{
    if (pica__reserve(ctx, 2))
        pica__write(ctx, reg, value);
}

void pica_write_reg_masked(pica_context* ctx, uint16_t reg, uint32_t mask, uint32_t value)
{
    if (pica__reserve(ctx, 2))
        pica__write_masked(ctx, reg, mask, value);
}

void pica_write_regs(pica_context* ctx, uint16_t first_reg, const uint32_t* values, unsigned count)
{
    if (pica__reserve(ctx, pica__cmd_words(count)))
        pica__write_inc(ctx, first_reg, values, count);
}

void pica_write_reg_repeat(pica_context* ctx, uint16_t reg, const uint32_t* values, unsigned count)
{
    if (pica__reserve(ctx, pica__cmd_words(count)))
        pica__write_rep(ctx, reg, values, count);
}
