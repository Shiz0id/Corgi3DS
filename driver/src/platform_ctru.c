/*
 * platform_ctru.c - libctru / gsp::Gpu backend for 3DS homebrew.
 *
 * Every engine operation is submitted through the GSP shared-memory command
 * queue and waited for synchronously via the matching GSP event.
 */
#if defined(__3DS__)

#include <3ds.h>
#include "internal.h"

static bool auto_flush = true;

void pica_ctru_auto_flush(bool enable)
{
    auto_flush = enable;
}

static uint32_t ctru_virt_to_phys(void* user, const void* ptr)
{
    (void)user;
    return osConvertVirtToPhys(ptr);
}

static void* ctru_alloc_linear(void* user, size_t size, size_t align)
{
    (void)user;
    if (align < 0x80)
        align = 0x80;
    return linearMemAlign(size, align);
}

static void ctru_free_linear(void* user, void* ptr)
{
    (void)user;
    linearFree(ptr);
}

static void* ctru_alloc_vram(void* user, size_t size)
{
    (void)user;
    return vramAlloc(size);
}

static void ctru_free_vram(void* user, void* ptr)
{
    (void)user;
    vramFree(ptr);
}

static void ctru_flush(void* user, const void* ptr, size_t size)
{
    (void)user;
    GSPGPU_FlushDataCache(ptr, size);
}

static void ctru_invalidate(void* user, const void* ptr, size_t size)
{
    (void)user;
    GSPGPU_InvalidateDataCache(ptr, size);
}

static int ctru_run_cmdlist(void* user, const uint32_t* buf, size_t size)
{
    (void)user;
    if (auto_flush)
    {
        extern u32 __ctru_linear_heap;
        GSPGPU_FlushDataCache((const void*)__ctru_linear_heap, envGetLinearHeapSize());
    }
    if (R_FAILED(GX_ProcessCommandList((u32*)buf, size, 0)))
        return -1;
    gspWaitForP3D();
    return 0;
}

static int ctru_memory_fill(void* user, void* start, size_t size, uint32_t value, unsigned width_bits)
{
    (void)user;
    u16 control = GX_FILL_TRIGGER;
    if (width_bits == 24)
        control |= GX_FILL_24BIT_DEPTH;
    else if (width_bits == 32)
        control |= GX_FILL_32BIT_DEPTH;
    if (R_FAILED(GX_MemoryFill((u32*)start, value, (u32*)((u8*)start + size), control, NULL, 0, NULL, 0)))
        return -1;
    gspWaitForPSC0();
    return 0;
}

static int ctru_display_transfer(void* user, const void* src, uint32_t src_dim, void* dst, uint32_t dst_dim,
                                 uint32_t flags)
{
    (void)user;
    if (R_FAILED(GX_DisplayTransfer((u32*)src, src_dim, (u32*)dst, dst_dim, flags)))
        return -1;
    gspWaitForPPF();
    return 0;
}

static int ctru_texture_copy(void* user, const void* src, uint32_t src_line, void* dst, uint32_t dst_line,
                             size_t size, uint32_t flags)
{
    (void)user;
    if (R_FAILED(GX_TextureCopy((u32*)src, src_line, (u32*)dst, dst_line, size, flags)))
        return -1;
    gspWaitForPPF();
    return 0;
}

static const pica_platform ctru_platform = {
    .user = NULL,
    .virt_to_phys = ctru_virt_to_phys,
    .alloc_linear = ctru_alloc_linear,
    .free_linear = ctru_free_linear,
    .alloc_vram = ctru_alloc_vram,
    .free_vram = ctru_free_vram,
    .flush_dcache = ctru_flush,
    .invalidate_dcache = ctru_invalidate,
    .run_cmdlist = ctru_run_cmdlist,
    .memory_fill = ctru_memory_fill,
    .display_transfer = ctru_display_transfer,
    .texture_copy = ctru_texture_copy,
};

const pica_platform* pica_platform_ctru(void)
{
    return &ctru_platform;
}

#endif /* __3DS__ */
