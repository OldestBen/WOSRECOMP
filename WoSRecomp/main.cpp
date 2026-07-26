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
#include <string>

#include <file.h>
#include <image.h>

#include "ppc_recomp_shared.h"
#include "import_log.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <sys/mman.h>
#endif

namespace
{

// Set by main() once the reservation exists, so the crash reporter can
// translate a faulting host address back into a guest address.
uint8_t* g_guestBase = nullptr;
uint64_t g_guestSize = 0;

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

// Section names come from IMAGE_SECTION_HEADER::Name, which is an 8-byte
// field that is NOT null-terminated when the name uses all 8 bytes. XenonUtils
// builds a std::string from it with `std::string(const char*)`, so an 8-char
// name runs on into whatever follows in the header — which is why "BINKDATA"
// prints as "BINKDATAh=" and ".XBMOVIE" drags in a newline. Cosmetic for us,
// but a garbled name in a diagnostic is a garbled diagnostic.
std::string CleanName(const std::string& raw)
{
    std::string out;
    for (size_t i = 0; i < raw.size() && i < 8; ++i)
    {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c < 0x20 || c > 0x7E)
            break;
        out += char(c);
    }
    return out.empty() ? "<unnamed>" : out;
}

#ifdef _WIN32

// Turn "Segmentation fault" into something diagnosable.
//
// Without this all we learn is that the process died. What we actually need
// is: which address faulted, was it a read or a write, and — since the guest
// address space is just `base + guest_address` — what guest address that
// corresponds to. A fault at guest 0x82... is the recompiled game touching
// unmapped memory; a fault outside the reservation entirely is a bug in the
// harness or in host code.
LONG WINAPI CrashReporter(EXCEPTION_POINTERS* info)
{
    const auto* rec = info->ExceptionRecord;

    const char* name = "unknown";
    switch (rec->ExceptionCode)
    {
    case EXCEPTION_ACCESS_VIOLATION:      name = "access violation"; break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:   name = "illegal instruction"; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    name = "integer divide by zero"; break;
    case EXCEPTION_STACK_OVERFLOW:        name = "stack overflow"; break;
    case EXCEPTION_PRIV_INSTRUCTION:      name = "privileged instruction"; break;
    case EXCEPTION_IN_PAGE_ERROR:         name = "in-page error"; break;
    default: break;
    }

    printf("\n=== CRASH: %s (0x%08lX) ===\n", name, rec->ExceptionCode);
    printf("faulting instruction at %p\n", rec->ExceptionAddress);

    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
    {
        const auto op = rec->ExceptionInformation[0];
        const auto addr = reinterpret_cast<uint8_t*>(rec->ExceptionInformation[1]);

        printf("tried to %s address %p\n",
            op == 0 ? "READ" : (op == 1 ? "WRITE" : "EXECUTE"), (void*)addr);

        if (g_guestBase != nullptr && addr >= g_guestBase && addr < g_guestBase + g_guestSize)
        {
            const uint64_t guest = uint64_t(addr - g_guestBase);
            printf("that is GUEST address 0x%08" PRIX64 "\n", guest);

            if (guest >= PPC_IMAGE_BASE + PPC_IMAGE_SIZE)
                printf("  -> inside the indirect-call table (a call through a\n"
                       "     function pointer we never populated)\n");
            else if (guest < PPC_IMAGE_BASE)
                printf("  -> below the image base: uncommitted low memory. Most\n"
                       "     likely a null/garbage guest pointer being dereferenced.\n");
            else
                printf("  -> inside the loaded image\n");
        }
        else
        {
            printf("that is OUTSIDE the guest reservation (%p .. %p)\n",
                (void*)g_guestBase, (void*)(g_guestBase + g_guestSize));
            printf("  -> host-side bug, not the recompiled game touching bad memory\n");
        }
    }

    wos::DumpImportLogUnsafe();
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

#endif // _WIN32

} // namespace

int main(int argc, char** argv)
{
    // Unbuffered, so the last line printed is genuinely the last line that
    // ran. Under MinTTY (Git Bash) stdout is a pipe, not a console, so the
    // default is *fully* buffered — on a crash the tail of the output is
    // lost and the visible stopping point is wherever the 4 KiB buffer last
    // flushed, which is not where the fault happened. Getting that wrong
    // sends you looking in the wrong function entirely.
    setvbuf(stdout, nullptr, _IONBF, 0);

#ifdef _WIN32
    SetUnhandledExceptionFilter(CrashReporter);
#endif

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

    g_guestBase = base;
    g_guestSize = kTotalSize;
    printf("guest base: %p\n\n", (void*)base);

    // Every section's source pointer is `image.data.get() + VirtualAddress`,
    // assigned by XenonUtils with no bounds check against the buffer it just
    // allocated. Those two numbers do not have to agree: for BASIC-compressed
    // XEXs the buffer is sized by summing the compression blocks, while
    // image.size is then overwritten with the header's imageSize. A section
    // near the top of the image can therefore point past the end of the
    // decrypted data, and memcpy'ing it reads unmapped memory.
    //
    // So: validate every source range before copying, and say so out loud.
    const uint8_t* const imgBegin = image.data.get();
    const uint8_t* const imgEnd = imgBegin + image.size;

    printf("decrypted image buffer: %p .. %p (0x%X bytes)\n",
        (const void*)imgBegin, (const void*)imgEnd, image.size);
    printf("%zu section(s)\n\n", image.sections.size());

    size_t badSections = 0;

    for (const auto& section : image.sections)
    {
        // Printed before any work, so if this faults the log names the
        // section that did it rather than the last one that survived.
        printf("  %-10s guest 0x%08zX  size 0x%-8X src %p  %-4s ",
            CleanName(section.name).c_str(), section.base, section.size,
            (const void*)section.data,
            (section.flags & SectionFlags_Code) ? "CODE" : "");

        if (section.base + section.size > kTotalSize)
        {
            printf("SKIPPED: extends past the guest reservation\n");
            ++badSections;
            continue;
        }

        uint64_t start = section.base;
        uint64_t size = section.size;
        PageAlign(start, size);

        if (!CommitRange(base, start, size))
            return EXIT_FAILURE;

        size_t copy = section.size;
        const char* note = "ok";

        if (section.data == nullptr)
        {
            copy = 0;
            note = "zero-filled (no source data)";
        }
        else if (section.data < imgBegin || section.data >= imgEnd)
        {
            copy = 0;
            note = "ZERO-FILLED: source lies outside the decrypted image";
            ++badSections;
        }
        else if (section.data + copy > imgEnd)
        {
            copy = size_t(imgEnd - section.data);
            note = "CLAMPED: source runs past the end of the decrypted image";
            ++badSections;
        }

        if (copy > 0)
            std::memcpy(base + section.base, section.data, copy);

        if (copy < section.size)
            printf("%s (copied 0x%zX of 0x%X)\n", note, copy, section.size);
        else
            printf("%s\n", note);
    }

    if (badSections > 0)
    {
        printf("\n%zu section(s) had an out-of-range source. That is an XEX\n", badSections);
        printf("parsing problem, not a game problem — see the comment above this\n");
        printf("loop. The image is mapped anyway, with the bad ranges zeroed.\n");
    }

    // Commit and populate the indirect-call table.
    printf("\ncommitting indirect-call table: guest 0x%" PRIX64 " .. 0x%" PRIX64 " (%.1f MiB)\n",
        kFuncTableOffset, kFuncTableOffset + kFuncTableSize,
        double(kFuncTableSize) / (1024.0 * 1024.0));

    if (!CommitRange(base, kFuncTableOffset, kFuncTableSize))
        return EXIT_FAILURE;

    printf("populating function table...\n");

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

    printf("function table: %zu mapped", mapped);
    if (outOfRange > 0)
        printf(", %zu outside code range (skipped)", outOfRange);
    printf("\n");

    printf("entry point: 0x%zX\n", image.entry_point);

    if (image.entry_point < PPC_CODE_BASE ||
        image.entry_point >= PPC_CODE_BASE + PPC_CODE_SIZE)
    {
        fprintf(stderr, "\nEntry point is outside the recompiled code range\n"
                        "(0x%llX .. 0x%llX) — cannot look it up.\n",
            (unsigned long long)PPC_CODE_BASE,
            (unsigned long long)(PPC_CODE_BASE + PPC_CODE_SIZE));
        return EXIT_FAILURE;
    }

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

    printf("entry resolved to host %p\n", (void*)entry);
    printf("\nscratch stack at 0x%X (0x%X bytes)\n", kStackTop, kStackSize);
    printf("calling entry point — expect a fault, nothing is set up yet\n");
    fflush(stdout);

    entry(ctx, base);

    wos::DumpImportLog();

    printf("\nReturned from the entry point without faulting.\n");
    printf("Given imports are stubbed to nop/blr, that most likely means it ran\n");
    printf("straight through doing nothing useful rather than actually working.\n");
    printf("r3 = 0x%llX\n", (unsigned long long)ctx.r3.u64);
    return EXIT_SUCCESS;
}
