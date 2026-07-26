#include "import_log.h"

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
std::unordered_map<std::string, uint64_t> g_counts;

} // namespace

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
