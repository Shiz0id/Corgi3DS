/*
 * platform_mmio.c - direct register backend (bare metal / emulator harness).
 */
#include <string.h>
#include "internal.h"
#include "pica/mmio.h"

/* ---- Arena allocator ----------------------------------------------------- */

static void* arena_alloc(pica_mmio_arena* a, size_t size, size_t align)
{
    uint32_t pos = 0;

    if (!a->virt || !size || a->num_blocks >= PICA_MMIO_MAX_BLOCKS)
        return NULL;
    if (align < 16)
        align = 16;
    size = (size + 15) & ~(size_t)15;

    for (unsigned i = 0; i <= a->num_blocks; i++)
    {
        /* Align the physical address, not just the offset */
        uint32_t start = (uint32_t)(((a->phys + pos + align - 1) & ~(uint32_t)(align - 1)) - a->phys);
        uint32_t limit = i < a->num_blocks ? a->blocks[i].offset : (uint32_t)a->size;
        if (start <= limit && limit - start >= size)
        {
            memmove(&a->blocks[i + 1], &a->blocks[i], (a->num_blocks - i) * sizeof(a->blocks[0]));
            a->blocks[i].offset = start;
            a->blocks[i].size = (uint32_t)size;
            a->num_blocks++;
            return a->virt + start;
        }
        if (i < a->num_blocks)
            pos = a->blocks[i].offset + a->blocks[i].size;
    }
    return NULL;
}

static bool arena_contains(const pica_mmio_arena* a, const void* ptr)
{
    const uint8_t* p = (const uint8_t*)ptr;
    return a->virt && p >= a->virt && p < a->virt + a->size;
}

static void arena_free(pica_mmio_arena* a, void* ptr)
{
    uint32_t off = (uint32_t)((uint8_t*)ptr - a->virt);
    for (unsigned i = 0; i < a->num_blocks; i++)
    {
        if (a->blocks[i].offset == off)
        {
            memmove(&a->blocks[i], &a->blocks[i + 1], (a->num_blocks - i - 1) * sizeof(a->blocks[0]));
            a->num_blocks--;
            return;
        }
    }
}

/* ---- Platform callbacks ---------------------------------------------------- */

static uint32_t mmio_virt_to_phys(void* user, const void* ptr)
{
    pica_mmio_desc* d = (pica_mmio_desc*)user;
    if (arena_contains(&d->linear, ptr))
        return d->linear.phys + (uint32_t)((const uint8_t*)ptr - d->linear.virt);
    if (arena_contains(&d->vram, ptr))
        return d->vram.phys + (uint32_t)((const uint8_t*)ptr - d->vram.virt);
    return 0;
}

static void* mmio_alloc_linear(void* user, size_t size, size_t align)
{
    return arena_alloc(&((pica_mmio_desc*)user)->linear, size, align);
}

static void mmio_free_linear(void* user, void* ptr)
{
    pica_mmio_desc* d = (pica_mmio_desc*)user;
    if (arena_contains(&d->linear, ptr))
        arena_free(&d->linear, ptr);
}

static void* mmio_alloc_vram(void* user, size_t size)
{
    return arena_alloc(&((pica_mmio_desc*)user)->vram, size, 0x80);
}

static void mmio_free_vram(void* user, void* ptr)
{
    pica_mmio_desc* d = (pica_mmio_desc*)user;
    if (arena_contains(&d->vram, ptr))
        arena_free(&d->vram, ptr);
}

static void mmio_flush(void* user, const void* ptr, size_t size)
{
    pica_mmio_desc* d = (pica_mmio_desc*)user;
    if (d->flush_dcache)
        d->flush_dcache(d->user, ptr, size);
}

static void mmio_invalidate(void* user, const void* ptr, size_t size)
{
    pica_mmio_desc* d = (pica_mmio_desc*)user;
    if (d->invalidate_dcache)
        d->invalidate_dcache(d->user, ptr, size);
}

/* Poll `offset` until done(value) holds.  Returns 0 on success. */
static int wait_for(pica_mmio_desc* d, uint32_t offset, uint32_t mask, uint32_t want)
{
    uint32_t polls = 0;
    while ((d->read32(d->user, offset) & mask) != want)
    {
        if (d->idle)
            d->idle(d->user);
        if (d->timeout_polls && ++polls >= d->timeout_polls)
            return -1;
    }
    return 0;
}

static int mmio_run_cmdlist(void* user, const uint32_t* buf, size_t size)
{
    pica_mmio_desc* d = (pica_mmio_desc*)user;
    uint32_t pa = mmio_virt_to_phys(user, buf);
    if (!pa || (pa & 7) || (size & 7))
        return -1;

    d->write32(d->user, PICA_MMIO_P3D_CMDBUF_SIZE, (uint32_t)(size >> 3));
    d->write32(d->user, PICA_MMIO_P3D_CMDBUF_ADDR, pa >> 3);
    d->write32(d->user, PICA_MMIO_P3D_CMDBUF_RUN, 1);
    return wait_for(d, PICA_MMIO_P3D_CMDBUF_RUN, 1, 0);
}

static int mmio_memory_fill(void* user, void* start, size_t size, uint32_t value, unsigned width_bits)
{
    pica_mmio_desc* d = (pica_mmio_desc*)user;
    uint32_t pa = mmio_virt_to_phys(user, start);
    uint32_t width = width_bits == 32 ? 2 : (width_bits == 24 ? 1 : 0);
    if (!pa)
        return -1;

    d->write32(d->user, PICA_MMIO_PSC_START(0), pa >> 3);
    d->write32(d->user, PICA_MMIO_PSC_END(0), (uint32_t)((pa + size) >> 3));
    d->write32(d->user, PICA_MMIO_PSC_VALUE(0), value);
    d->write32(d->user, PICA_MMIO_PSC_CONTROL(0), PICA_PSC_START | PICA_PSC_WIDTH(width));
    int res = wait_for(d, PICA_MMIO_PSC_CONTROL(0), PICA_PSC_FINISHED, PICA_PSC_FINISHED);
    d->write32(d->user, PICA_MMIO_PSC_CONTROL(0), 0);
    return res;
}

static int start_transfer(pica_mmio_desc* d)
{
    d->write32(d->user, PICA_MMIO_DMA_CONTROL, 1);
    return wait_for(d, PICA_MMIO_DMA_CONTROL, 0x100, 0x100);
}

static int mmio_display_transfer(void* user, const void* src, uint32_t src_dim, void* dst, uint32_t dst_dim,
                                 uint32_t flags)
{
    pica_mmio_desc* d = (pica_mmio_desc*)user;
    uint32_t in = mmio_virt_to_phys(user, src), out = mmio_virt_to_phys(user, dst);
    if (!in || !out)
        return -1;

    d->write32(d->user, PICA_MMIO_DMA_INPUT_ADDR, in >> 3);
    d->write32(d->user, PICA_MMIO_DMA_OUTPUT_ADDR, out >> 3);
    d->write32(d->user, PICA_MMIO_DMA_OUTPUT_DIM, dst_dim);
    d->write32(d->user, PICA_MMIO_DMA_INPUT_DIM, src_dim);
    d->write32(d->user, PICA_MMIO_DMA_FLAGS, flags);
    return start_transfer(d);
}

static int mmio_texture_copy(void* user, const void* src, uint32_t src_line, void* dst, uint32_t dst_line,
                             size_t size, uint32_t flags)
{
    pica_mmio_desc* d = (pica_mmio_desc*)user;
    uint32_t in = mmio_virt_to_phys(user, src), out = mmio_virt_to_phys(user, dst);
    if (!in || !out)
        return -1;

    d->write32(d->user, PICA_MMIO_DMA_INPUT_ADDR, in >> 3);
    d->write32(d->user, PICA_MMIO_DMA_OUTPUT_ADDR, out >> 3);
    d->write32(d->user, PICA_MMIO_DMA_TEXCOPY_SIZE, (uint32_t)size);
    d->write32(d->user, PICA_MMIO_DMA_TEXCOPY_IN_LINE, src_line);
    d->write32(d->user, PICA_MMIO_DMA_TEXCOPY_OUT_LINE, dst_line);
    d->write32(d->user, PICA_MMIO_DMA_FLAGS, flags | PICA_XFER_RAW_COPY);
    return start_transfer(d);
}

int pica_platform_mmio(pica_platform* out, pica_mmio_desc* desc)
{
    if (!out || !desc || !desc->write32 || !desc->read32)
        return PICA_ERR_ARG;

    memset(out, 0, sizeof(*out));
    out->user = desc;
    out->virt_to_phys = mmio_virt_to_phys;
    out->alloc_linear = mmio_alloc_linear;
    out->free_linear = mmio_free_linear;
    out->alloc_vram = mmio_alloc_vram;
    out->free_vram = mmio_free_vram;
    out->flush_dcache = mmio_flush;
    out->invalidate_dcache = mmio_invalidate;
    out->run_cmdlist = mmio_run_cmdlist;
    out->memory_fill = mmio_memory_fill;
    out->display_transfer = mmio_display_transfer;
    out->texture_copy = mmio_texture_copy;
    return PICA_OK;
}
