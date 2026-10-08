/*
 * pica/mmio.h - direct register backend.
 *
 * Drives the GPU's external registers (memory fill, transfer engine,
 * command-list DMA) directly, without gsp::Gpu.  Intended for
 *
 *   - bare-metal ARM11 code with an identity mapping of 0x10400000, VRAM and
 *     FCRAM (pass accessors that dereference volatile pointers), and
 *   - the host test harness, which routes the accessors into Corgi3DS's
 *     PICA200 emulation.
 *
 * The backend includes a small first-fit allocator for the two memory
 * arenas you hand it.  It assumes the LCDs have already been brought up
 * (e.g. by the bootloader); it does not program LCD timings.
 */
#ifndef PICA_MMIO_H
#define PICA_MMIO_H

#include "pica.h"

#define PICA_MMIO_MAX_BLOCKS 256

typedef struct pica_mmio_arena
{
    uint8_t* virt;
    uint32_t phys;
    size_t   size;
    struct
    {
        uint32_t offset;
        uint32_t size;
    } blocks[PICA_MMIO_MAX_BLOCKS]; /* allocated blocks, sorted by offset */
    unsigned num_blocks;
} pica_mmio_arena;

struct pica_mmio_desc
{
    void* user;

    /* External register access; `offset` is relative to 0x10400000. */
    void     (*write32)(void* user, uint32_t offset, uint32_t value);
    uint32_t (*read32)(void* user, uint32_t offset);
    /* Called while polling for completion (may be NULL).  The host harness
     * advances the emulator here. */
    void     (*idle)(void* user);
    /* Optional cache maintenance */
    void     (*flush_dcache)(void* user, const void* ptr, size_t size);
    void     (*invalidate_dcache)(void* user, const void* ptr, size_t size);
    /* Give up waiting for an engine after this many idle() calls (0 = never). */
    uint32_t timeout_polls;

    pica_mmio_arena linear; /* FCRAM (or any GPU-visible memory) */
    pica_mmio_arena vram;
};

/* Convenience initialiser for a descriptor's memory arenas. */
static inline void pica_mmio_set_arenas(pica_mmio_desc* d, void* linear_virt, uint32_t linear_phys,
                                        size_t linear_size, void* vram_virt, uint32_t vram_phys, size_t vram_size)
{
    d->linear.virt = (uint8_t*)linear_virt;
    d->linear.phys = linear_phys;
    d->linear.size = linear_size;
    d->linear.num_blocks = 0;
    d->vram.virt = (uint8_t*)vram_virt;
    d->vram.phys = vram_phys;
    d->vram.size = vram_size;
    d->vram.num_blocks = 0;
}

#endif /* PICA_MMIO_H */
