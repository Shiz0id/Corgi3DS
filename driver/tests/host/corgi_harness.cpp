/*
 * corgi_harness.cpp - runs the driver test suite against Corgi3DS's PICA200
 * emulation, using the driver's direct-MMIO backend.
 *
 * Only the GPU core is linked; the handful of Emulator / Scheduler /
 * MPCore_PMR methods it calls are provided here, backed by a flat model of
 * physical memory (VRAM at 0x18000000, FCRAM at 0x20000000).
 *
 *   corgi_pica_tests [--filter NAME] [--dump DIR] [--verbose]
 */
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <string>

#include "core/emulator.hpp"
#include "core/scheduler.hpp"
#include "core/arm11/gpu.hpp"
#include "core/arm11/mpcore_pmr.hpp"
#include "core/common/exceptions.hpp"

extern "C" {
#include "pica/mmio.h"
#include "suite.h"
}

static const uint32_t VRAM_PHYS = 0x18000000, VRAM_SIZE = 0x00600000;
static const uint32_t FCRAM_PHYS = 0x20000000, FCRAM_SIZE = 0x04000000;

static uint8_t* vram;
static uint8_t* fcram;

static uint8_t* phys_ptr(uint32_t addr, uint32_t size)
{
    if (addr >= VRAM_PHYS && addr + size <= VRAM_PHYS + VRAM_SIZE)
        return vram + (addr - VRAM_PHYS);
    if (addr >= FCRAM_PHYS && addr + size <= FCRAM_PHYS + FCRAM_SIZE)
        return fcram + (addr - FCRAM_PHYS);
    fprintf(stderr, "[harness] GPU access to unmapped physical address %08X\n", addr);
    static uint8_t dummy[8];
    memset(dummy, 0, sizeof(dummy));
    return dummy;
}

/* ---- Minimal implementations of what gpu.cpp links against ---------------- */

uint8_t Emulator::arm11_read8(int, uint32_t addr) { return *phys_ptr(addr, 1); }
uint16_t Emulator::arm11_read16(int, uint32_t addr) { uint16_t v; memcpy(&v, phys_ptr(addr, 2), 2); return v; }
uint32_t Emulator::arm11_read32(int, uint32_t addr) { uint32_t v; memcpy(&v, phys_ptr(addr, 4), 4); return v; }
void Emulator::arm11_write8(int, uint32_t addr, uint8_t v) { *phys_ptr(addr, 1) = v; }
void Emulator::arm11_write16(int, uint32_t addr, uint16_t v) { memcpy(phys_ptr(addr, 2), &v, 2); }
void Emulator::arm11_write32(int, uint32_t addr, uint32_t v) { memcpy(phys_ptr(addr, 4), &v, 4); }

static std::deque<std::pair<std::function<void(uint64_t)>, uint64_t>> pending_events;

void Scheduler::add_event(std::function<void(uint64_t)> func, int64_t, uint64_t, uint64_t param)
{
    pending_events.emplace_back(func, param);
}

void MPCore_PMR::assert_hw_irq(int) {}

/* ---- MMIO backend glue --------------------------------------------------- */

static GPU* gpu;
static bool failed_fatally;

static void mmio_write(void*, uint32_t offset, uint32_t value)
{
    try { gpu->write32(offset, value); }
    catch (EmuException::FatalError& e) { fprintf(stderr, "[harness] Corgi fatal error: %s\n", e.what()); failed_fatally = true; }
}

static uint32_t mmio_read(void*, uint32_t offset)
{
    return gpu->read32(offset);
}

static void mmio_idle(void*)
{
    if (pending_events.empty())
        return;
    auto ev = pending_events.front();
    pending_events.pop_front();
    try { ev.first(ev.second); }
    catch (EmuException::FatalError& e)
    {
        fprintf(stderr, "[harness] Corgi fatal error: %s\n", e.what());
        failed_fatally = true;
    }
}

/* ---- Suite I/O ------------------------------------------------------------- */

static std::string dump_dir;

static void log_msg(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void dump(const char* name, const uint32_t* px, unsigned w, unsigned h)
{
    if (dump_dir.empty())
        return;
    std::string path = dump_dir + "/" + name + ".ppm";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (unsigned i = 0; i < w * h; i++)
    {
        uint8_t rgb[3] = { (uint8_t)(px[i] >> 24), (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char** argv)
{
    const char* filter = nullptr;
    bool verbose = false;
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--filter") && i + 1 < argc)
            filter = argv[++i];
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc)
            dump_dir = argv[++i];
        else if (!strcmp(argv[i], "--verbose"))
            verbose = true;
        else
        {
            fprintf(stderr, "usage: %s [--filter NAME] [--dump DIR] [--verbose]\n", argv[0]);
            return 2;
        }
    }
    /* Corgi's GPU logs every register write to stdout */
    if (!verbose)
        if (!freopen("/dev/null", "w", stdout))
            return 2;

    vram = (uint8_t*)calloc(1, VRAM_SIZE);
    fcram = (uint8_t*)calloc(1, FCRAM_SIZE);

    /* The GPU only uses these pointers to call the methods defined above. */
    static uint64_t dummy[64];
    gpu = new GPU(reinterpret_cast<Emulator*>(dummy), reinterpret_cast<Scheduler*>(dummy),
                  reinterpret_cast<MPCore_PMR*>(dummy));
    gpu->reset(vram);

    static pica_mmio_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.write32 = mmio_write;
    desc.read32 = mmio_read;
    desc.idle = mmio_idle;
    desc.timeout_polls = 1000;
    pica_mmio_set_arenas(&desc, fcram, FCRAM_PHYS, FCRAM_SIZE, vram, VRAM_PHYS, VRAM_SIZE);

    pica_platform plat;
    pica_platform_mmio(&plat, &desc);

    suite_io io = { log_msg, dump, filter };
    int failures = suite_run(&plat, &io);
    if (failed_fatally)
        fprintf(stderr, "[harness] note: Corgi's emulation hit a fatal (unimplemented) path during the run\n");
    return failures == 0 ? 0 : 1;
}
