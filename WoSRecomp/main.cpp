// Minimal host harness: map the guest address space, load the XEX image into
// it, build the indirect-call table, and enter the game at its entry point.
//
// The point of this is NOT to run the game — it is to see how far it gets and
// where it stops.
//
// NOTE, having tested it: this DOES link, and undefined symbols are not the
// way to find missing imports. Every recompiled function is emitted as a weak
// alias, so nothing is ever undefined, and XenonUtils additionally rewrites
// each import thunk to nop/nop/nop/blr — so an unimplemented import returns
// immediately instead of failing. Convenient for bring-up, but it means
// missing functionality fails *silently*.
//
// The real list of what the runtime must implement comes from the XEX import
// table:
//
//     tools/xex_info/build/xex_info private/default.xex --imports
//
// To override a recompiled stub, define a strong function with the same name
// (e.g. __imp__XamLoaderLaunchTitle); it beats the weak alias at link time.
//
// See docs/03-runtime-architecture.md.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cinttypes>

#include <file.h>
#include <image.h>

#include "ppc_recomp_shared.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <sys/mman.h>
#endif

namespace
{

// Guest memory must cover [0, PPC_IMAGE_BASE + PPC_IMAGE_SIZE), because the
// generated code addresses it as base + guest_address with no translation.
// Above that sits the indirect-call table: PPC_LOOKUP_FUNC indexes it as
// (addr - PPC_CODE_BASE) * 2, i.e. 8 bytes of function pointer per 4-byte
// instruction slot.
constexpr uint64_t kFuncTableOffset = PPC_IMAGE_BASE + PPC_IMAGE_SIZE;
constexpr uint64_t kFuncTableSize   = PPC_CODE_SIZE * 2;
constexpr uint64_t kTotalSize       = kFuncTableOffset + kFuncTableSize;

// Reserve the whole range but commit only what we touch. The bottom ~2 GiB
// below the image base is never used — committing it would waste real memory
// for nothing.
uint8_t* ReserveGuestMemory()
{
#ifdef _WIN32
    auto* p = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kTotalSize, MEM_RESERVE, PAGE_READWRITE));
    if (p == nullptr)
        fprintf(stderr, "VirtualAlloc reserve failed: %lu\n", GetLastError());
    return p;
#else
    void* p = mmap(nullptr, kTotalSize, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
    {
        perror("mmap reserve");
        return nullptr;
    }
    return static_cast<uint8_t*>(p);
#endif
}

bool CommitRange(uint8_t* base, uint64_t offset, uint64_t size)
{
#ifdef _WIN32
    if (VirtualAlloc(base + offset, size, MEM_COMMIT, PAGE_READWRITE) == nullptr)
    {
        fprintf(stderr, "VirtualAlloc commit failed at 0x%" PRIx64 " (+0x%" PRIx64 "): %lu\n",
            offset, size, GetLastError());
        return false;
    }
    return true;
#else
    if (mprotect(base + offset, size, PROT_READ | PROT_WRITE) != 0)
    {
        perror("mprotect commit");
        return false;
    }
    return true;
#endif
}

// Round [start, start+size) out to page boundaries so commits never straddle.
void PageAlign(uint64_t& start, uint64_t& size, uint64_t pageSize = 0x10000)
{
    const uint64_t end = (start + size + pageSize - 1) & ~(pageSize - 1);
    start &= ~(pageSize - 1);
    size = end - start;
}

} // namespace

int main(int argc, char** argv)
{
    const char* xexPath = argc > 1 ? argv[1] : "private/default.xex";

    printf("=== WoSRecomp harness ===\n");
    printf("image base 0x%llX  size 0x%llX\n", (unsigned long long)PPC_IMAGE_BASE, (unsigned long long)PPC_IMAGE_SIZE);
    printf("code  base 0x%llX  size 0x%llX\n", (unsigned long long)PPC_CODE_BASE, (unsigned long long)PPC_CODE_SIZE);
    printf("guest reservation: %.2f GiB\n\n", double(kTotalSize) / (1024.0 * 1024.0 * 1024.0));

    const auto file = LoadFile(xexPath);
    if (file.empty())
    {
        fprintf(stderr, "Failed to read \"%s\".\n", xexPath);
        fprintf(stderr, "Pass the path explicitly, or run from the repo root.\n");
        return EXIT_FAILURE;
    }

    Image image;
    try
    {
        image = Image::ParseImage(file.data(), file.size());
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "Failed to parse image: %s\n", e.what());
        return EXIT_FAILURE;
    }

    uint8_t* base = ReserveGuestMemory();
    if (base == nullptr)
        return EXIT_FAILURE;

    // Map each section at its virtual address.
    for (const auto& section : image.sections)
    {
        uint64_t start = section.base;
        uint64_t size = section.size;
        PageAlign(start, size);

        if (!CommitRange(base, start, size))
            return EXIT_FAILURE;

        if (section.data != nullptr)
            std::memcpy(base + section.base, section.data, section.size);

        printf("mapped %-12s 0x%08zX  size 0x%-8X %s\n",
            section.name.c_str(), section.base, section.size,
            (section.flags & SectionFlags_Code) ? "CODE" : "");
    }

    // Commit and populate the indirect-call table.
    if (!CommitRange(base, kFuncTableOffset, kFuncTableSize))
        return EXIT_FAILURE;

    size_t mapped = 0, outOfRange = 0;
    for (const PPCFuncMapping* m = PPCFuncMappings; m->host != nullptr; ++m)
    {
        if (m->guest < PPC_CODE_BASE || m->guest >= PPC_CODE_BASE + PPC_CODE_SIZE)
        {
            ++outOfRange;
            continue;
        }
        *reinterpret_cast<PPCFunc**>(
            base + kFuncTableOffset + (uint64_t(uint32_t(m->guest) - PPC_CODE_BASE) * 2)) = m->host;
        ++mapped;
    }

    printf("\nfunction table: %zu mapped", mapped);
    if (outOfRange > 0)
        printf(", %zu outside code range (skipped)", outOfRange);
    printf("\n");

    printf("entry point: 0x%zX\n", image.entry_point);

    PPCFunc* entry = *reinterpret_cast<PPCFunc**>(
        base + kFuncTableOffset + (uint64_t(uint32_t(image.entry_point) - PPC_CODE_BASE) * 2));

    if (entry == nullptr)
    {
        fprintf(stderr, "\nNo recompiled function at the entry point.\n");
        fprintf(stderr, "That means the recompiler never emitted it — check the\n");
        fprintf(stderr, "function boundary config around 0x%zX.\n", image.entry_point);
        return EXIT_FAILURE;
    }

    // A real runtime would set up a guest stack, thread state, TLS and the
    // kernel export table here. None of that exists, so this will most likely
    // fault — the interesting part is how far it gets first.
    //
    // r1 is the stack pointer. Point it at a scratch region inside the guest
    // space rather than 0, so the very first prologue store doesn't fault
    // before executing a single useful instruction.
    constexpr uint32_t kStackTop = uint32_t(PPC_IMAGE_BASE) - 0x1000;
    constexpr uint32_t kStackSize = 0x40000;
    if (!CommitRange(base, kStackTop - kStackSize, kStackSize + 0x1000))
    {
        fprintf(stderr, "Failed to commit a scratch stack.\n");
        return EXIT_FAILURE;
    }

    PPCContext ctx{};
    ctx.r1.u64 = kStackTop;

    printf("\nscratch stack at 0x%X (0x%X bytes)\n", kStackTop, kStackSize);
    printf("calling entry point — expect a fault, nothing is set up yet\n");
    fflush(stdout);

    entry(ctx, base);

    printf("\nReturned from the entry point without faulting.\n");
    printf("Given imports are stubbed to nop/blr, that most likely means it ran\n");
    printf("straight through doing nothing useful rather than actually working.\n");
    printf("r3 = 0x%llX\n", (unsigned long long)ctx.r3.u64);
    return EXIT_SUCCESS;
}
