#include "import_log.h"

#include <algorithm>
#include <cstdio>
#include <cinttypes>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace wos
{
namespace
{

std::mutex g_mutex;

// Insertion-ordered, so the trace reflects the boot sequence rather than
// alphabetical order. First-call order is the interesting signal.
std::vector<const char*> g_order;

// Keyed by the literal's ADDRESS, not its text.
//
// Every WOS_IMPORT_STUB call site passes a string literal, and each import
// name appears exactly once in the binary — the WOS_IMPL_ guards make sure a
// name is either a generated stub or a kernel implementation, never both. So
// the pointer is a stable unique key, and using it removes a std::string
// construction and a string hash from a path taken tens of millions of times
// during startup.
//
// That matters beyond speed: this runs under a global mutex on every import
// call from every guest thread, and stack dumps repeatedly caught threads
// inside `operator new` here. Measurement apparatus that serialises the thing
// it measures is not measuring the thing.
//
// If the assumption ever breaks, the symptom is benign and visible: the same
// name appearing twice in the import list.
std::unordered_map<const char*, uint64_t> g_counts;

// Bounded on purpose: the key includes a guest-controlled address, and this is
// reached from paths running millions of times a second.
constexpr size_t kMaxCallSites = 48;

struct CallSite
{
    const char* name = nullptr;
    uint32_t callSite = 0;
    uint32_t lastDetail = 0;
    uint64_t calls = 0;
};

std::mutex g_callSiteMutex;
std::vector<CallSite> g_callSites;

} // namespace

void LogCallSite(const char* name, uint32_t callSite, uint32_t detail)
{
    std::lock_guard<std::mutex> lock(g_callSiteMutex);

    for (auto& site : g_callSites)
    {
        if (site.callSite == callSite && site.name == name)
        {
            site.lastDetail = detail;
            ++site.calls;
            return;
        }
    }

    if (g_callSites.size() >= kMaxCallSites)
        return;

    g_callSites.push_back({ name, callSite, detail, 1 });
}

void ReportCallSites()
{
    std::vector<CallSite> rows;
    {
        std::lock_guard<std::mutex> lock(g_callSiteMutex);
        rows = g_callSites;
    }

    if (rows.empty())
        return;

    std::sort(rows.begin(), rows.end(),
        [](const CallSite& a, const CallSite& b) { return a.calls > b.calls; });

    printf("[callsites] %zu site(s):\n", rows.size());
    for (size_t i = 0; i < rows.size() && i < 6; ++i)
    {
        // callSite is the return address; the `bl` is four bytes back, which is
        // the address to feed to --disasm.
        printf("    bl@0x%08X -> %-28s %llu call(s), last arg %u (0x%X)\n",
            rows[i].callSite - 4, rows[i].name,
            (unsigned long long)rows[i].calls,
            rows[i].lastDetail, rows[i].lastDetail);
    }
}

void LogImportCall(const char* name)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    auto [it, inserted] = g_counts.try_emplace(name, 0);
    ++it->second;

    if (inserted)
    {
        g_order.push_back(name);
        // Print immediately: if the game faults, whatever it called last is
        // the most valuable clue, and a buffered summary would be lost.
        printf("[import %3zu] %s\n", g_order.size(), name);
        fflush(stdout);
    }
}

namespace
{

void DumpLocked()
{
    printf("\n=== import trace ===\n");

    if (g_order.empty())
    {
        printf("No imports were called at all.\n");
        printf("The game didn't get far enough to ask the kernel for anything —\n");
        printf("suspect the entry point or context setup rather than the imports.\n");
        return;
    }

    const size_t total = WoSImportStubCount();
    printf("%zu of %zu import(s) reached (%.1f%%), in this order:\n\n",
        g_order.size(), total, total ? (100.0 * double(g_order.size()) / double(total)) : 0.0);

    for (size_t i = 0; i < g_order.size(); ++i)
        printf("  %3zu. %-44s x%" PRIu64 "\n", i + 1, g_order[i], g_counts[g_order[i]]);

    printf("\nThese are the functions to implement first — in this order.\n");
    printf("Everything else is unreachable until these behave properly.\n");
}

} // namespace

void DumpImportLog()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    DumpLocked();
}

std::vector<std::pair<const char*, uint64_t>> SnapshotImportCounts()
{
    std::lock_guard<std::mutex> lock(g_mutex);

    std::vector<std::pair<const char*, uint64_t>> out;
    out.reserve(g_order.size());
    for (const char* name : g_order)
        out.emplace_back(name, g_counts[name]);
    return out;
}

void DumpImportLogUnsafe()
{
    // try_to_lock, not lock: a fault inside LogImportCall leaves this thread
    // already holding g_mutex, and std::mutex is not recursive, so blocking
    // here would deadlock the crash handler. The data is only appended to, so
    // reading it without the lock can at worst show a torn tail — acceptable
    // when the alternative is printing nothing at all.
    std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
    DumpLocked();
}

} // namespace wos
