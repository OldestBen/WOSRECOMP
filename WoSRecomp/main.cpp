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

#include "guest.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cinttypes>
#include <string>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <file.h>
#include <image.h>

#include "ppc_recomp_shared.h"
#include "import_log.h"
#include "object.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <dbghelp.h>
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

// ...but the guest may address anywhere in 32 bits, so RESERVE all of it.
//
// PPC_LOAD_U32 is `__builtin_bswap32(*(volatile uint32_t*)(base + (x)))` —
// no masking, no bounds check. The Xbox 360 maps physical memory at
// 0x80000000+ with aliases up through 0xFFFFFFFF, and the game does reach
// them: the first run faulted reading guest 0x93010000, which is 0x93010000
// past a reservation that stopped at 0x842585C8.
//
// Reserving the full 4 GiB costs address space, not memory — nothing is
// committed until touched. Getting this wrong turns an ordinary guest access
// into an unexplained crash outside the reservation, which is exactly how it
// was misdiagnosed the first time.
constexpr uint64_t kGuestReserve = PPC_MEMORY_SIZE;
static_assert(kGuestReserve >= kTotalSize, "guest reservation must cover the function table");

// Scratch guest stack, just below the image. Grows downward.
constexpr uint32_t kStackTop  = uint32_t(PPC_IMAGE_BASE) - 0x1000;
constexpr uint32_t kStackSize = 0x40000;

// How far the guest stack may grow before we call it a runaway rather than a
// deep call tree. Real startup code does not need megabytes.
constexpr uint64_t kStackRunawayLimit = 2ull << 20;

// Everything within this much of kStackTop is treated as stack for the
// purpose of runaway detection. Generous, because we do not know how the
// guest's own allocator will lay things out later.
constexpr uint64_t kStackRegionSpan = 256ull << 20;

uint8_t* ReserveGuestMemory()
{
#ifdef _WIN32
    auto* p = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kGuestReserve, MEM_RESERVE, PAGE_READWRITE));
    if (p == nullptr)
        fprintf(stderr, "VirtualAlloc reserve failed: %lu\n", GetLastError());
    return p;
#else
    void* p = mmap(nullptr, kGuestReserve, PROT_NONE,
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

// Put the FP control word back to something safe before reporting anything.
//
// Reporting does floating-point work — the import trace prints a percentage —
// and we may well be *here* because FP exceptions are unmasked. That is not
// hypothetical: the run that first got a working allocator died with
// EXCEPTION_FLT_INEXACT_RESULT, and the reporter then printed the
// "=== import trace ===" header and nothing after it, because the very next
// thing it did was compute `100.0 * reached / total` and trap again.
void RestoreHostFpState()
{
#if defined(__x86_64__) || defined(_M_X64)
    simde_mm_setcsr(0x1F80);   // round-to-nearest, every FP exception masked
#endif
}

#ifdef _WIN32

// Commit guest pages on first touch.
//
// The guest's own allocator hands out addresses across the whole 32-bit space
// and nothing pre-commits them. Without this, the first access to any address
// we didn't map by hand is fatal, and the game stops within a few thousand
// instructions of the entry point — long before it asks the kernel for
// anything, which is the thing we actually want to observe.
//
// The cost is that a genuinely wild pointer now reads zeros and lets the game
// wander on rather than stopping where the mistake was. That trade is right
// for bring-up and wrong later, so: every distinct region is logged, there is
// a hard ceiling on how much gets committed, and WOS_NO_AUTOCOMMIT=1 turns it
// off to get a hard fault at the first bad access.
std::atomic<uint64_t> g_autoCommitBytes{0};
std::atomic<uint64_t> g_autoCommitCount{0};
std::atomic<uint64_t> g_stackCommitBytes{0};
bool g_autoCommit = true;

// Every guest-executing thread, so any of them can be backtraced on demand.
//
// CaptureStackBackTrace only ever walks the *calling* thread, which has been a
// real limitation: twice now a diagnostic has named a thread by inference and
// been wrong. Suspending a thread and walking its stack answers the question
// directly instead.
std::mutex g_threadRegistryMutex;
std::vector<std::pair<HANDLE, std::string>> g_threadRegistry;

void RegisterGuestThreadForBacktrace(const char* label)
{
    HANDLE dup = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_threadRegistryMutex);
    g_threadRegistry.emplace_back(dup, label);
}

// Name the recompiled functions currently on the host stack.
//
// Recompiled guest functions are ordinary C++ functions called normally, so
// the host stack *is* the guest call stack, and a runaway guest stack shows
// up as a deep host stack full of sub_XXXXXXXX frames. Symbolising the top of
// it turns "something recurses" into the actual cycle, which is the only
// question worth answering here. RelWithDebInfo gives us the PDB for free.
void PrintGuestBacktrace(unsigned frameCount = 40)
{
    static constexpr unsigned kMaxFrames = 96;
    void* frames[kMaxFrames];
    const USHORT captured = CaptureStackBackTrace(
        1, frameCount < kMaxFrames ? frameCount : kMaxFrames, frames, nullptr);

    if (captured == 0)
    {
        printf("  (no frames captured — recompiled code may lack unwind info)\n");
        return;
    }

    const HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(proc, nullptr, TRUE);

    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = MAX_SYM_NAME;

    for (USHORT i = 0; i < captured; ++i)
    {
        DWORD64 disp = 0;
        if (SymFromAddr(proc, reinterpret_cast<DWORD64>(frames[i]), &disp, sym))
            printf("  #%-3u %s +0x%llX\n", i, sym->Name, (unsigned long long)disp);
        else
            printf("  #%-3u %p  (no symbol)\n", i, frames[i]);
    }

    SymCleanup(proc);
}

// Walk one suspended thread's stack and symbolise it.
//
// StackWalk64 rather than CaptureStackBackTrace, because the target is another
// thread: it needs an explicit CONTEXT, which only makes sense while the
// thread is stopped.
void BacktraceSuspendedThread(HANDLE thread, const char* label, unsigned maxFrames)
{
    printf("  --- %s ---\n", label);

    if (SuspendThread(thread) == DWORD(-1))
    {
        printf("    (could not suspend)\n");
        return;
    }

    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;

    if (!GetThreadContext(thread, &context))
    {
        printf("    (could not read context)\n");
        ResumeThread(thread);
        return;
    }

    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    const HANDLE proc = GetCurrentProcess();

    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = MAX_SYM_NAME;

    for (unsigned i = 0; i < maxFrames; ++i)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &frame, &context,
                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
        {
            break;
        }
        if (frame.AddrPC.Offset == 0)
            break;

        DWORD64 disp = 0;
        if (SymFromAddr(proc, frame.AddrPC.Offset, &disp, sym))
            printf("    #%-3u %s +0x%llX\n", i, sym->Name, (unsigned long long)disp);
        else
            printf("    #%-3u 0x%llX  (no symbol)\n", i,
                (unsigned long long)frame.AddrPC.Offset);
    }

    ResumeThread(thread);
}

// Backtrace every registered guest thread except the caller.
void DumpAllGuestThreadStacks(unsigned maxFrames)
{
    const HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(proc, nullptr, TRUE);

    const DWORD self = GetCurrentThreadId();

    std::lock_guard<std::mutex> lock(g_threadRegistryMutex);
    printf("\n=== all guest thread stacks (%zu) ===\n", g_threadRegistry.size());

    for (auto& [handle, label] : g_threadRegistry)
    {
        if (GetThreadId(handle) == self)
        {
            printf("  --- %s (this thread, skipped) ---\n", label.c_str());
            continue;
        }
        BacktraceSuspendedThread(handle, label.c_str(), maxFrames);
    }

    SymCleanup(proc);
}

// Called when the guest stack has grown past anything plausible. Reports and
// exits, rather than letting the host stack overflow — a stack overflow
// leaves no room to run an exception filter, so the process just dies
// silently and the whole trace is lost. That is exactly what happened on the
// run that first reached 15 imports.
[[noreturn]] void ReportRunawayStack(uint64_t guest)
{
    RestoreHostFpState();
    printf("\n=== RUNAWAY GUEST STACK ===\n");
    printf("The guest stack has grown past %llu MiB (now at 0x%08" PRIX64 ", started at 0x%08X).\n",
        (unsigned long long)(kStackRunawayLimit >> 20), guest, kStackTop);
    printf("That is unbounded recursion, not a deep call tree.\n\n");
    printf("Recompiled functions on the stack, innermost first:\n");
    PrintGuestBacktrace();
    printf("\nRepeated names above are the cycle. The usual cause at this stage\n");
    printf("is an import stub that returns nothing, so the caller reads a stale\n");
    printf("register as a result and retries forever.\n");

    wos::DumpImportLogUnsafe();
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 3);
    __builtin_unreachable();
}

constexpr uint64_t kAutoCommitGranularity = 0x10000;   // 64 KiB
constexpr uint64_t kAutoCommitCeiling     = 512ull << 20;
constexpr uint64_t kAutoCommitLogLimit    = 48;

LONG WINAPI GuestPageCommitter(EXCEPTION_POINTERS* info)
{
    const auto* rec = info->ExceptionRecord;

    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;

    auto* addr = reinterpret_cast<uint8_t*>(rec->ExceptionInformation[1]);

    if (!g_autoCommit || g_guestBase == nullptr ||
        addr < g_guestBase || addr >= g_guestBase + g_guestSize)
    {
        return EXCEPTION_CONTINUE_SEARCH;   // not ours — let the reporter have it
    }

    if (g_autoCommitBytes.load(std::memory_order_relaxed) >= kAutoCommitCeiling)
        return EXCEPTION_CONTINUE_SEARCH;   // runaway; stop and report

    const uint64_t guest = uint64_t(addr - g_guestBase);
    const uint64_t region = guest & ~(kAutoCommitGranularity - 1);

    if (VirtualAlloc(g_guestBase + region, kAutoCommitGranularity,
                     MEM_COMMIT, PAGE_READWRITE) == nullptr)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    g_autoCommitBytes.fetch_add(kAutoCommitGranularity, std::memory_order_relaxed);
    const uint64_t n = g_autoCommitCount.fetch_add(1, std::memory_order_relaxed) + 1;

    // Growth below the initial scratch stack, in the stack's neighbourhood.
    if (region < kStackTop && region + kStackRegionSpan >= kStackTop)
    {
        const uint64_t grown =
            g_stackCommitBytes.fetch_add(kAutoCommitGranularity, std::memory_order_relaxed)
            + kAutoCommitGranularity;

        if (grown >= kStackRunawayLimit)
            ReportRunawayStack(guest);
    }

    if (n <= kAutoCommitLogLimit)
    {
        printf("[guest page] commit 0x%08" PRIX64 "  (first touched 0x%08" PRIX64 ", %s)\n",
            region, guest, rec->ExceptionInformation[0] == 0 ? "read" : "write");
    }
    else if (n == kAutoCommitLogLimit + 1)
    {
        printf("[guest page] ... further commits not logged individually\n");
    }

    return EXCEPTION_CONTINUE_EXECUTION;
}

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
    RestoreHostFpState();

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
    // Floating point. These mean the FP exception masks in MXCSR got cleared:
    // the guest expects PowerPC semantics, where MSR[FE0,FE1] leave FP traps
    // disabled and results are simply rounded.
    case EXCEPTION_FLT_DENORMAL_OPERAND:  name = "FP denormal operand"; break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:    name = "FP divide by zero"; break;
    case EXCEPTION_FLT_INEXACT_RESULT:    name = "FP inexact result"; break;
    case EXCEPTION_FLT_INVALID_OPERATION: name = "FP invalid operation"; break;
    case EXCEPTION_FLT_OVERFLOW:          name = "FP overflow"; break;
    case EXCEPTION_FLT_STACK_CHECK:       name = "FP stack check"; break;
    case EXCEPTION_FLT_UNDERFLOW:         name = "FP underflow"; break;
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

            if (guest >= kFuncTableOffset && guest < kFuncTableOffset + kFuncTableSize)
                printf("  -> inside the indirect-call table (a call through a\n"
                       "     function pointer we never populated)\n");
            else if (guest >= PPC_IMAGE_BASE && guest < PPC_IMAGE_BASE + PPC_IMAGE_SIZE)
                printf("  -> inside the loaded image\n");
            else if (guest >= 0x80000000ull)
                printf("  -> Xbox 360 physical-memory alias space. Normal for the\n"
                       "     game to use; it faulted because nothing has been\n"
                       "     committed there and auto-commit is off or capped.\n");
            else
                printf("  -> guest user address space. Most likely a null or\n"
                       "     garbage pointer, since no allocator exists yet.\n");
        }
        else if (reinterpret_cast<uintptr_t>(addr) < 0x10000)
        {
            printf("that is a NULL-ish pointer.\n");
            if (op == 8)
                printf("  -> almost certainly a call through an empty slot in the\n"
                       "     indirect-call table: the game branched to a guest\n"
                       "     address the recompiler never emitted a function for.\n");
            else
                printf("  -> a null pointer dereference in host code.\n");
        }
        else
        {
            // Note this only means host-side because the reservation now spans
            // the full 32-bit guest range. It used to stop at ~2.06 GiB, which
            // made an ordinary guest access to 0x93010000 look like a harness
            // bug — a wrong verdict stated confidently.
            printf("that is OUTSIDE the guest reservation (%p .. %p)\n",
                (void*)g_guestBase, (void*)(g_guestBase + g_guestSize));
            printf("  -> host-side bug: the reservation covers the entire 32-bit\n");
            printf("     guest space, so this cannot be a guest access.\n");
            // Signed offset from the guest base. A near miss just below the
            // base is a very different bug from a wild pointer, and the raw
            // addresses alone make that hard to see.
            const uintptr_t faulting = reinterpret_cast<uintptr_t>(addr);
            const uintptr_t guestBase = reinterpret_cast<uintptr_t>(g_guestBase);
            const bool below = faulting < guestBase;
            printf("     Offset from the guest base: %s0x%llX\n",
                below ? "-" : "+",
                (unsigned long long)(below ? guestBase - faulting : faulting - guestBase));
        }
    }

    // The stack of the faulting thread.
    //
    // Without this a crash reports an address and nothing else, which names
    // neither the guest function nor the host code that went wrong — the last
    // one cost a whole round trip. The filter runs on the faulting thread, so
    // a plain capture of the current stack is the right one; the top few
    // frames are this reporter and can be read past.
    printf("\n=== faulting thread stack ===\n");
    wos::PrintGuestStack(32);

    if (g_autoCommitCount.load() > 0)
    {
        printf("\nauto-committed %" PRIu64 " guest region(s), %" PRIu64 " MiB total\n",
            g_autoCommitCount.load(), g_autoCommitBytes.load() >> 20);
        if (g_autoCommitBytes.load() >= kAutoCommitCeiling)
            printf("HIT THE %llu MiB CEILING — the game is probably scribbling\n"
                   "over random addresses rather than allocating sensibly.\n",
                (unsigned long long)(kAutoCommitCeiling >> 20));
    }

    wos::DumpImportLogUnsafe();
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

#endif // _WIN32

} // namespace

// Declared in kernel/guest.h so kernel code can end a run deliberately.
//
// It must not throw or return: it is called from inside recompiled guest
// frames, which have no exception handling and no idea how to unwind. Print
// everything worth knowing, then stop the process where it stands.
namespace wos
{
void RegisterThreadForBacktrace(const char* label)
{
#ifdef _WIN32
    RegisterGuestThreadForBacktrace(label);
#else
    (void)label;
#endif
}

void DumpAllThreadStacks(unsigned frames)
{
#ifdef _WIN32
    RestoreHostFpState();
    DumpAllGuestThreadStacks(frames);
#else
    (void)frames;
#endif
}

void PrintGuestStack(unsigned frames)
{
#ifdef _WIN32
    PrintGuestBacktrace(frames);
#else
    (void)frames;
    printf("  (no backtrace support on this platform)\n");
#endif
}

[[noreturn]] void FatalGuestStop(const char* reason)
{
    RestoreHostFpState();
    printf("\n=== STOPPED: %s ===\n", reason);

#ifdef _WIN32
    printf("\nRecompiled functions on the stack, innermost first:\n");
    PrintGuestBacktrace();
#endif

    DumpImportLogUnsafe();
    fflush(stdout);

#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 2);
#endif
    _Exit(2);
}
} // namespace wos

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
    g_autoCommit = (GetEnvironmentVariableA("WOS_NO_AUTOCOMMIT", nullptr, 0) == 0);
    // First in the chain, so it sees the fault before the unhandled filter.
    AddVectoredExceptionHandler(1, GuestPageCommitter);
    SetUnhandledExceptionFilter(CrashReporter);
#endif

    const char* xexPath = argc > 1 ? argv[1] : "private/default.xex";

    printf("=== WoSRecomp harness ===\n");
    printf("image base 0x%llX  size 0x%llX\n", (unsigned long long)PPC_IMAGE_BASE, (unsigned long long)PPC_IMAGE_SIZE);
    printf("code  base 0x%llX  size 0x%llX\n", (unsigned long long)PPC_CODE_BASE, (unsigned long long)PPC_CODE_SIZE);
    printf("guest reservation: %.2f GiB (mapped content ends at 0x%" PRIX64 ")\n\n",
        double(kGuestReserve) / (1024.0 * 1024.0 * 1024.0), kTotalSize);

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
    g_guestSize = kGuestReserve;
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

        if (section.base + section.size > kGuestReserve)
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
    if (!CommitRange(base, kStackTop - kStackSize, kStackSize + 0x1000))
    {
        fprintf(stderr, "Failed to commit a scratch stack.\n");
        return EXIT_FAILURE;
    }

    PPCContext ctx{};
    ctx.r1.u64 = kStackTop;

    // r13 is the per-thread block pointer, not a scratch register — see
    // kernel/pcr.cpp. Leaving it 0 makes every load through it read the zero
    // page, which is what wedged the graphics layer in a wait that could
    // never time out.
    ctx.r13.u64 = wos::CreateThreadPcr(base);

    // Seed the FP control word from the host's actual MXCSR.
    //
    // Without this, ctx.fpscr.csr starts at 0 (value-initialised), and the
    // first enableFlushMode() does `csr |= FlushMask; setcsr(csr)` — writing
    // MXCSR = 0x8040. Bits 7..12 are the exception *masks*, where 1 means
    // masked, so that clears every one of them: the next inexact FP result
    // raises EXCEPTION_FLT_INEXACT_RESULT (0xC000008F) instead of rounding.
    //
    // loadFromHost() copies the real MXCSR (0x1F80 by default, all masked),
    // so enabling flush mode yields 0x9FC0 — masks preserved, FTZ and DAZ
    // set, which is what the guest actually wants. PowerPC has FP traps
    // disabled by default via MSR[FE0,FE1], so the game never expects them.
    ctx.fpscr.loadFromHost();

    // Heartbeat.
    //
    // A game that boots and then runs is indistinguishable, on a silent
    // terminal, from a game that boots and then hangs — the import log only
    // prints on *first* call, so a steady state prints nothing at all. This
    // reports what changed since the last tick, which answers the only
    // question that matters: is it doing work, or spinning?
    wos::RegisterThreadForBacktrace("main thread");

    std::thread([]
    {
        auto previous = wos::SnapshotImportCounts();
        int quietTicks = 0;
        bool dumped = false;

        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::seconds(5));

            auto current = wos::SnapshotImportCounts();

            std::unordered_map<std::string, uint64_t> before;
            for (const auto& [name, count] : previous)
                before[name] = count;

            std::vector<std::pair<const char*, uint64_t>> deltas;
            uint64_t totalDelta = 0;
            for (const auto& [name, count] : current)
            {
                const uint64_t was = before.count(name) ? before[name] : 0;
                if (count > was)
                {
                    deltas.emplace_back(name, count - was);
                    totalDelta += count - was;
                }
            }

            if (deltas.empty())
            {
                printf("[heartbeat] no import activity in the last 5 s — "
                       "%zu import(s) reached, likely spinning\n", current.size());
            }
            else
            {
                std::sort(deltas.begin(), deltas.end(),
                    [](const auto& a, const auto& b) { return a.second > b.second; });

                printf("[heartbeat] %llu call(s) across %zu import(s) in 5 s; busiest:",
                    (unsigned long long)totalDelta, deltas.size());
                for (size_t i = 0; i < deltas.size() && i < 4; ++i)
                    printf(" %s x%llu", deltas[i].first, (unsigned long long)deltas[i].second);
                printf("\n");
            }

            wos::ReportWaitActivity();

            // "Busy" here means *new* imports appearing, not calls happening.
            // A game polling the same three waits forever is not progressing,
            // and after a while the only remaining question is what each
            // thread is actually executing.
            if (current.size() == previous.size())
                ++quietTicks;
            else
                quietTicks = 0;

            if (quietTicks == 3 && !dumped)
            {
                dumped = true;
                printf("\n[watchdog] no new imports for 15 s — dumping every "
                       "thread's stack.\nThis is the only way to see a thread "
                       "running guest code that calls nothing.\n");
                wos::DumpAllThreadStacks(16);
                printf("=== end of thread stacks ===\n\n");
            }

            previous = std::move(current);
        }
    }).detach();

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
