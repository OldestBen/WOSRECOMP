// Prints structural metadata (base address, entry point, section layout) for
// a Xbox 360 XEX/XEXP file. Reuses XenonUtils' own loader (the same code
// XenonAnalyse/XenonRecomp use), so decryption/decompression of retail XEXs
// is handled automatically.
//
// Deliberately does NOT dump any code/data bytes or copyrighted content —
// only the structural metadata needed to start filling in the config TOML
// (see docs/02-config-guide.md).
#include <cstdio>
#include <cstring>
#include <vector>
#include <map>
#include <string>
#include <algorithm>
#include <file.h>
#include <image.h>
#include <xex.h>
#include <xbox.h>
#include <byteswap.h>
#include <fstream>
#include <sstream>
#include <string>
#include <toml++/toml.hpp>
#include <ppc.h>
#include <disasm.h>
#include <cstdlib>

// ---------------------------------------------------------------------------
// --helpers: locate the compiler-generated register save/restore functions
//
// XenonRecomp needs the addresses of eight helpers (__savegprlr_14 and
// friends) and has no autodetection for them, so they're a manual per-game
// lookup — see docs/02-config-guide.md.
//
// They can't be found by byte-searching the XEX file itself: a retail XEX is
// encrypted and compressed, so these patterns don't appear in the raw bytes.
// They only exist in the decrypted, decompressed image, which is what
// Image::ParseImage produces — hence doing the search here.
//
// PPC instructions are 4-byte aligned and fixed width, so the scan steps by 4.
// The 4-byte signatures are single instructions and can occur incidentally, so
// each is validated against the *next* instruction in the sequence: these
// helpers save/restore r14..r31 in order, so instruction N+1 is the same
// opcode with the register incremented by 1 and the displacement by 8. That
// makes a false positive very unlikely.
// ---------------------------------------------------------------------------

struct HelperPattern
{
    const char* configField;
    const char* symbol;
    const char* disasm;
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> next;  // empty = no follow-on validation
};

static const std::vector<HelperPattern>& helperPatterns()
{
    // Primary signatures are from XenonRecomp's README. The `next` sequences
    // are derived from the PPC encodings: same opcode, register +1,
    // displacement +8.
    static const std::vector<HelperPattern> patterns = {
        { "restgprlr_14_address", "__restgprlr_14", "ld r14, -0x98(r1)",
          { 0xE9, 0xC1, 0xFF, 0x68 }, { 0xE9, 0xE1, 0xFF, 0x70 } },
        { "savegprlr_14_address", "__savegprlr_14", "std r14, -0x98(r1)",
          { 0xF9, 0xC1, 0xFF, 0x68 }, { 0xF9, 0xE1, 0xFF, 0x70 } },
        { "restfpr_14_address",   "__restfpr_14",   "lfd f14, -0x90(r12)",
          { 0xC9, 0xCC, 0xFF, 0x70 }, { 0xC9, 0xEC, 0xFF, 0x78 } },
        { "savefpr_14_address",   "__savefpr_14",   "stfd f14, -0x90(r12)",
          { 0xD9, 0xCC, 0xFF, 0x70 }, { 0xD9, 0xEC, 0xFF, 0x78 } },
        // The VMX signatures are already two instructions, so they're
        // specific enough without extra validation.
        { "restvmx_14_address",   "__restvmx_14",   "li r11,-0x120 / lvx v14,r11,r12",
          { 0x39, 0x60, 0xFE, 0xE0, 0x7D, 0xCB, 0x60, 0xCE }, {} },
        { "savevmx_14_address",   "__savevmx_14",   "li r11,-0x120 / stvx v14,r11,r12",
          { 0x39, 0x60, 0xFE, 0xE0, 0x7D, 0xCB, 0x61, 0xCE }, {} },
        { "restvmx_64_address",   "__restvmx_64",   "li r11,-0x400 / lvx128 v64,r11,r12",
          { 0x39, 0x60, 0xFC, 0x00, 0x10, 0x0B, 0x60, 0xCB }, {} },
        { "savevmx_64_address",   "__savevmx_64",   "li r11,-0x400 / stvx128 v64,r11,r12",
          { 0x39, 0x60, 0xFC, 0x00, 0x10, 0x0B, 0x61, 0xCB }, {} },
    };
    return patterns;
}

static int findHelpers(const Image& image)
{
    printf("Scanning CODE sections for register save/restore helpers...\n\n");

    std::vector<std::pair<const HelperPattern*, size_t>> found;
    int missing = 0;

    for (const auto& pat : helperPatterns())
    {
        std::vector<size_t> hits;

        for (const auto& section : image.sections)
        {
            if (!(section.flags & SectionFlags_Code) || section.data == nullptr)
                continue;

            const size_t total = pat.bytes.size() + pat.next.size();
            if (section.size < total)
                continue;

            for (size_t off = 0; off + total <= section.size; off += 4)
            {
                if (std::memcmp(section.data + off, pat.bytes.data(), pat.bytes.size()) != 0)
                    continue;

                if (!pat.next.empty() &&
                    std::memcmp(section.data + off + pat.bytes.size(),
                                pat.next.data(), pat.next.size()) != 0)
                {
                    continue;  // matched the instruction, but not the sequence
                }

                hits.push_back(section.base + off);
            }
        }

        if (hits.empty())
        {
            printf("  %-22s %-16s NOT FOUND\n", pat.configField, pat.symbol);
            printf("  %-22s %-16s   (%s)\n", "", "", pat.disasm);
            ++missing;
        }
        else
        {
            printf("  %-22s %-16s 0x%08zX%s\n",
                pat.configField, pat.symbol, hits.front(),
                hits.size() > 1 ? "   <-- AMBIGUOUS" : "");
            if (hits.size() > 1)
            {
                printf("      %zu candidates:", hits.size());
                for (size_t i = 0; i < hits.size() && i < 8; ++i)
                    printf(" 0x%08zX", hits[i]);
                if (hits.size() > 8)
                    printf(" ...");
                printf("\n");
            }
            found.emplace_back(&pat, hits.front());
        }
    }

    printf("\n");

    if (!found.empty())
    {
        printf("--- paste into WoSRecompLib/config/WoS_config.toml ---\n\n");
        for (const auto& [pat, addr] : found)
            printf("%s = 0x%08zX\n", pat->configField, addr);
        printf("\n");
    }

    if (missing > 0)
    {
        printf("%d helper(s) not found. That can be legitimate — a game only\n", missing);
        printf("contains the helpers it actually uses (e.g. no VMX helpers if it\n");
        printf("never saves vector registers). Omit missing entries from the config.\n");
    }

    return EXIT_SUCCESS;
}

// ---------------------------------------------------------------------------
// --fix-switches: derive `functions = [...]` overrides for switch tables whose
// cases jump outside their detected function.
//
// XenonRecomp registers one function per .pdata unwind record
// (recompiler.cpp, using FunctionLength * 4). A single logical function can be
// split across several consecutive .pdata records, so a switch that jumps
// between the pieces looks like it's jumping out of bounds, and the
// recompiler reports:
//     ERROR: Switch case at <site> is trying to jump outside function: <target>
//
// The fix is a `functions` entry that spans the whole thing. This computes
// them: for each switch site, find the .pdata record containing it, and if any
// case target lands beyond that record's end, widen the function to cover the
// furthest target — absorbing the intervening records, which is precisely what
// "these are really one function" means.
// ---------------------------------------------------------------------------

struct PdataFunc { uint32_t begin; uint32_t end; };

// Read the big-endian instruction word at a virtual address, if mapped in a
// CODE section.
static bool readInsn(const Image& image, uint32_t addr, uint32_t& out)
{
    for (const auto& s : image.sections)
    {
        if (!(s.flags & SectionFlags_Code) || s.data == nullptr)
            continue;
        if (addr < s.base || addr + 4 > s.base + s.size)
            continue;
        uint32_t raw;
        std::memcpy(&raw, s.data + (addr - s.base), 4);
        out = ByteSwap(raw);
        return true;
    }
    return false;
}

// Instructions that end a function: blr, bctr, and unconditional non-linking
// branches. (bl/bctrl are calls, not terminators.)
static bool isTerminator(uint32_t insn)
{
    if (insn == 0x4E800020) return true;   // blr
    if (insn == 0x4E800420) return true;   // bctr
    if (PPC_OP(insn) == 18 && !PPC_BL(insn)) return true;  // b / ba
    return false;
}

// Walk FORWARD from `from` to just past the next terminator, giving a real
// function boundary.
//
// This matters more than it looks. recompiler.cpp walks the code section
// linearly, advancing `base` by each known function's size and calling
// Function::Analyze wherever no function starts. Function::Analyze begins at
// size 0 and returns 0 if its block stack empties immediately — and the caller
// then does `base += fn.size`, i.e. adds nothing. A single function whose end
// lands mid-instruction-stream can therefore wedge the recompiler in an
// infinite loop with no output at all.
//
// So a declared function must end where a function plausibly ends, not at an
// arbitrary offset like "furthest switch target + 4".
static uint32_t findFunctionEnd(const Image& image, uint32_t from, uint32_t ceiling)
{
    constexpr uint32_t kMaxScan = 0x8000;
    const uint32_t limit = std::min(ceiling, from + kMaxScan);

    for (uint32_t a = from; a < limit; a += 4)
    {
        uint32_t insn;
        if (!readInsn(image, a, insn))
            break;
        if (isTerminator(insn))
            return a + 4;   // end is just past the terminator
    }
    return 0;
}

// Walk backwards from `site` to the terminator of the preceding function, then
// forward over any padding, to get the start of the function containing
// `site`. `floorAddr` is a hard lower bound (end of the nearest preceding
// .pdata record) so we never wander into a function we already know about.
// Returns 0 if no plausible start is found.
static uint32_t inferFunctionStart(const Image& image, uint32_t site, uint32_t floorAddr)
{
    constexpr uint32_t kMaxScan = 0x8000;  // 32 KiB back-scan cap
    const uint32_t limit = std::max(floorAddr, site > kMaxScan ? site - kMaxScan : 0u);

    for (uint32_t a = site - 4; a >= limit && a < site; a -= 4)
    {
        uint32_t insn;
        if (!readInsn(image, a, insn))
            return 0;

        if (isTerminator(insn))
        {
            // Function starts after the terminator, skipping zero padding.
            uint32_t start = a + 4;
            uint32_t pad;
            while (start < site && readInsn(image, start, pad) && pad == 0)
                start += 4;
            return start < site ? start : 0;
        }
    }

    // Hit the floor without finding a terminator: if that floor is a known
    // record boundary, the function almost certainly starts exactly there.
    return (floorAddr != 0 && floorAddr < site) ? floorAddr : 0;
}

static std::vector<PdataFunc> readPdata(const Image& image)
{
    std::vector<PdataFunc> out;
    const Section* pdata = image.Find(".pdata");
    if (pdata == nullptr || pdata->data == nullptr)
        return out;

    const size_t count = pdata->size / sizeof(IMAGE_CE_RUNTIME_FUNCTION);
    const auto* pf = reinterpret_cast<const IMAGE_CE_RUNTIME_FUNCTION*>(pdata->data);
    out.reserve(count);

    for (size_t i = 0; i < count; ++i)
    {
        IMAGE_CE_RUNTIME_FUNCTION fn = pf[i];
        fn.BeginAddress = ByteSwap(fn.BeginAddress);
        fn.Data = ByteSwap(fn.Data);
        if (fn.BeginAddress == 0 || fn.FunctionLength == 0)
            continue;
        out.push_back({ fn.BeginAddress, fn.BeginAddress + fn.FunctionLength * 4u });
    }

    std::sort(out.begin(), out.end(),
        [](const PdataFunc& a, const PdataFunc& b) { return a.begin < b.begin; });
    return out;
}

static int fixSwitches(const Image& image, const char* switchTomlPath, const char* writeConfigPath)
{
    const std::vector<PdataFunc> pdata = readPdata(image);
    if (pdata.empty())
    {
        fprintf(stderr, "No usable .pdata records found — cannot derive boundaries.\n");
        return EXIT_FAILURE;
    }
    printf("Loaded %zu .pdata function records.\n", pdata.size());

    toml::table tbl;
    try
    {
        tbl = toml::parse_file(switchTomlPath);
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "Failed to parse \"%s\": %s\n", switchTomlPath, e.what());
        return EXIT_FAILURE;
    }

    const auto* switches = tbl["switch"].as_array();
    if (switches == nullptr)
    {
        fprintf(stderr, "\"%s\" contains no [[switch]] entries.\n", switchTomlPath);
        return EXIT_FAILURE;
    }
    printf("Loaded %zu switch tables.\n\n", switches->size());

    // Locate the .pdata record containing an address.
    auto containing = [&](uint32_t addr) -> const PdataFunc* {
        auto it = std::upper_bound(pdata.begin(), pdata.end(), addr,
            [](uint32_t v, const PdataFunc& f) { return v < f.begin; });
        if (it == pdata.begin())
            return nullptr;
        --it;
        return (addr < it->end) ? &*it : nullptr;
    };

    // fnStart -> required end address
    std::map<uint32_t, uint32_t> widened;
    size_t offending = 0, unlocatable = 0, inferred = 0, unresolved = 0;

    for (const auto& node : *switches)
    {
        const auto* s = node.as_table();
        if (s == nullptr)
            continue;

        const auto base = (*s)["base"].value<int64_t>();
        if (!base)
            continue;
        const auto site = static_cast<uint32_t>(*base);

        // Collect every target this switch can reach.
        std::vector<uint32_t> targets;
        if (auto def = (*s)["default"].value<int64_t>())
            targets.push_back(static_cast<uint32_t>(*def));
        if (const auto* labels = (*s)["labels"].as_array())
        {
            for (const auto& l : *labels)
            {
                if (auto v = l.value<int64_t>())
                    targets.push_back(static_cast<uint32_t>(*v));
            }
        }
        if (targets.empty())
            continue;

        const PdataFunc* fn = containing(site);
        if (fn == nullptr)
        {
            // No unwind record covers this site. .pdata does not describe
            // every function — leaf functions that never unwind can be
            // omitted — so these live in the gaps between records and are
            // discovered by XenonRecomp's heuristic branch scan instead,
            // which is what sizes them too small.
            //
            // Recover the real start by walking backwards from the switch to
            // the previous function's terminator (blr / bctr / unconditional
            // b), then skipping any padding. Clamp to the preceding .pdata
            // record's end so an inferred function can never overlap a known
            // one.
            ++unlocatable;

            uint32_t maxTarget = 0;
            for (uint32_t t : targets)
                maxTarget = std::max(maxTarget, t);

            // Lower bound: end of the nearest record finishing before `site`.
            uint32_t floorAddr = 0;
            for (const auto& f : pdata)
            {
                if (f.end <= site)
                    floorAddr = std::max(floorAddr, f.end);
                else if (f.begin > site)
                    break;
            }

            const uint32_t start = inferFunctionStart(image, site, floorAddr);
            if (start == 0)
            {
                ++unresolved;
                continue;
            }

            // Don't run past the next known record.
            uint32_t ceilingAddr = UINT32_MAX;
            for (const auto& f : pdata)
            {
                if (f.begin > site) { ceilingAddr = f.begin; break; }
            }

            // End at a real function boundary, not at an arbitrary offset.
            // Ending mid-stream makes recompiler.cpp's linear walk analyse
            // garbage, which can yield a zero-size function and hang it.
            uint32_t end = findFunctionEnd(image,
                std::max(maxTarget + 4u, site + 4u), ceilingAddr);
            if (end == 0)
                end = ceilingAddr;   // no terminator found: stop at the next known function
            if (end > ceilingAddr)
                end = ceilingAddr;   // never swallow a known function

            ++inferred;
            auto [it2, ins2] = widened.emplace(start, end);
            if (!ins2)
                it2->second = std::max(it2->second, end);
            continue;
        }

        uint32_t needEnd = fn->end;
        bool outside = false;
        for (uint32_t t : targets)
        {
            // A target below the function start is a different problem
            // (tail-call / shared tail); only widening forward is safe here.
            if (t >= fn->end)
            {
                outside = true;
                needEnd = std::max(needEnd, t + 4u);
            }
        }

        if (!outside)
            continue;

        // Snap to a real function boundary — see findFunctionEnd.
        {
            uint32_t ceilingAddr = UINT32_MAX;
            for (const auto& f : pdata)
                if (f.begin > site) { ceilingAddr = f.begin; break; }
            const uint32_t snapped = findFunctionEnd(image, needEnd, ceilingAddr);
            needEnd = snapped ? snapped : std::min(needEnd, ceilingAddr);
        }

        ++offending;
        auto [it, inserted] = widened.emplace(fn->begin, needEnd);
        if (!inserted)
            it->second = std::max(it->second, needEnd);
    }

    printf("Switch sites whose cases escape their .pdata function: %zu\n", offending);
    printf("Switch sites with no containing .pdata record:          %zu\n", unlocatable);
    printf("  ...of those, function start inferred by disassembly:  %zu\n", inferred);
    if (unresolved > 0)
        printf("  ...unresolved (no terminator found, skipped):        %zu\n", unresolved);
    printf("Distinct functions to declare:                          %zu\n\n", widened.size());

    if (widened.empty())
    {
        printf("Nothing to do.\n");
        return EXIT_SUCCESS;
    }

    // Merge overlapping extents.
    //
    // inferFunctionStart walks back to the first terminator it sees, but an
    // early `blr` is a *return*, not a function boundary — functions commonly
    // have several exit points. So two switch sites in the same function can
    // yield different starts, the later one landing just after an early
    // return. The signature is unmistakable: the entries overlap and share an
    // end address. Whenever extents overlap they belong to one function, so
    // take the earliest start and the furthest end.
    std::vector<std::pair<uint32_t, uint32_t>> merged;
    size_t mergeCount = 0;
    for (const auto& [start, end] : widened)   // std::map => ascending by start
    {
        if (!merged.empty() && start < merged.back().second)
        {
            merged.back().second = std::max(merged.back().second, end);
            ++mergeCount;
        }
        else
        {
            merged.emplace_back(start, end);
        }
    }

    if (mergeCount > 0)
    {
        printf("Merged %zu overlapping extent(s) into their enclosing function\n", mergeCount);
        printf("(multiple switches in one function, split by early returns).\n");
        printf("Final function count: %zu\n\n", merged.size());
    }

    printf("--- replace the `functions = [...]` block in WoS_config.toml ---\n\n");
    printf("functions = [\n");
    for (const auto& [start, end] : merged)
    {
        const PdataFunc* orig = containing(start);
        size_t absorbed = 0;
        for (const auto& f : pdata)
        {
            if (f.begin > start && f.begin < end)
                ++absorbed;
        }
        printf("    { address = 0x%08X, size = 0x%X },%s%s\n",
            start, end - start,
            orig ? "  # from .pdata" : "  # inferred (not in .pdata)",
            absorbed ? "  WARNING: spans known record(s)" : "");
    }
    printf("]\n\n");
    if (writeConfigPath != nullptr)
    {
        std::ifstream in(writeConfigPath, std::ios::binary);
        if (!in)
        {
            fprintf(stderr, "Cannot open \"%s\" for reading.\n", writeConfigPath);
            return EXIT_FAILURE;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        std::string cfg = ss.str();
        in.close();

        const size_t start = cfg.find("functions = [");
        if (start == std::string::npos)
        {
            fprintf(stderr, "No `functions = [` block found in \"%s\".\n", writeConfigPath);
            return EXIT_FAILURE;
        }
        const size_t close = cfg.find(']', start);
        if (close == std::string::npos)
        {
            fprintf(stderr, "Unterminated `functions = [` block in \"%s\".\n", writeConfigPath);
            return EXIT_FAILURE;
        }

        std::string block = "functions = [\n";
        for (const auto& [s2, e2] : merged)
        {
            char line[96];
            snprintf(line, sizeof(line), "    { address = 0x%08X, size = 0x%X },\n", s2, e2 - s2);
            block += line;
        }
        block += "]";

        // Keep a backup before overwriting.
        const std::string bak = std::string(writeConfigPath) + ".bak";
        { std::ofstream b(bak, std::ios::binary); b << cfg; }

        cfg.replace(start, close - start + 1, block);
        std::ofstream out(writeConfigPath, std::ios::binary);
        if (!out)
        {
            fprintf(stderr, "Cannot open \"%s\" for writing.\n", writeConfigPath);
            return EXIT_FAILURE;
        }
        out << cfg;
        out.close();

        printf("Wrote %zu entries into %s (backup: %s)\n\n",
            merged.size(), writeConfigPath, bak.c_str());
    }

    printf("Each entry spans from the function's start to its furthest switch\n");
    printf("target. Starts inside .pdata come from the unwind record; starts in\n");
    printf("the gaps between records are recovered by scanning back to the\n");
    printf("previous function's terminator, clamped so they can never overlap a\n");
    printf("known record. Re-run XenonRecomp afterwards; config entries are\n");
    printf("registered before .pdata, so these take priority.\n");

    return EXIT_SUCCESS;
}

// ---------------------------------------------------------------------------
// --imports: list the kernel/OS functions the game imports.
//
// This is the runtime to-do list, taken from the game rather than guessed.
// XenonUtils resolves the XEX import table during Image::ParseImage and
// inserts a named symbol (__imp__XamFoo, __imp__NtBar) for every import whose
// ordinal it recognises, so they are simply the __imp__-prefixed symbols.
//
// Note it also overwrites each import thunk with nop/nop/nop/blr. That is why
// the host links and runs without implementing any of these: an unimplemented
// import returns immediately instead of failing. Convenient for bring-up,
// but it means missing imports fail *silently* — the game will misbehave
// rather than tell you what it needed.
// ---------------------------------------------------------------------------
static int listImports(const Image& image)
{
    std::vector<std::pair<std::string, uint32_t>> imports;
    for (const auto& sym : image.symbols)
    {
        if (sym.name.rfind("__imp__", 0) == 0)
            imports.emplace_back(sym.name, static_cast<uint32_t>(sym.address));
    }

    if (imports.empty())
    {
        printf("No recognised imports found.\n");
        printf("Either the XEX has no import table, or none of its ordinals are\n");
        printf("in XenonUtils' xam.xex / xboxkrnl.exe tables.\n");
        return EXIT_SUCCESS;
    }

    std::sort(imports.begin(), imports.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });

    // Group by the prefix after __imp__, which maps closely to subsystem.
    auto subsystem = [](const std::string& n) -> const char* {
        const char* s = n.c_str() + 7;  // skip "__imp__"
        if (strncmp(s, "Xam", 3) == 0)  return "Xam (system/UI)";
        if (strncmp(s, "Xe", 2) == 0)   return "Xe (GPU)";
        if (strncmp(s, "Nt", 2) == 0)   return "Nt (kernel objects)";
        if (strncmp(s, "Rtl", 3) == 0)  return "Rtl (runtime library)";
        if (strncmp(s, "Ke", 2) == 0)   return "Ke (kernel core)";
        if (strncmp(s, "Ex", 2) == 0)   return "Ex (executive)";
        if (strncmp(s, "Ob", 2) == 0)   return "Ob (object manager)";
        if (strncmp(s, "Mm", 2) == 0)   return "Mm (memory manager)";
        if (strncmp(s, "Vd", 2) == 0)   return "Vd (video driver)";
        if (strncmp(s, "Io", 2) == 0)   return "Io (file I/O)";
        if (strncmp(s, "XAudio", 6) == 0 || strncmp(s, "XMA", 3) == 0) return "Audio";
        if (strncmp(s, "XNet", 4) == 0) return "XNet (networking)";
        return "other";
    };

    std::map<std::string, std::vector<const std::pair<std::string, uint32_t>*>> groups;
    for (const auto& imp : imports)
        groups[subsystem(imp.first)].push_back(&imp);

    printf("%zu imported function(s) across %zu subsystem(s).\n", imports.size(), groups.size());
    printf("Each needs a host implementation to override the recompiled stub.\n\n");

    for (const auto& [group, entries] : groups)
    {
        printf("--- %s (%zu) ---\n", group.c_str(), entries.size());
        for (const auto* e : entries)
            printf("    0x%08X  %s\n", e->second, e->first.c_str());
        printf("\n");
    }

    printf("Implement these as strong definitions with the same names; the\n");
    printf("recompiled versions are weak aliases, so yours win at link time.\n");
    return EXIT_SUCCESS;
}

// ---------------------------------------------------------------------------
// --emit-stubs: generate logging stubs for every import.
//
// Turns the static import list into a *dynamic* one. Each stub logs its own
// name the first time it's called, so running the host prints the actual boot
// sequence — which imports the game reaches, in what order, and where it
// stops. That is far more actionable than 214 names sorted alphabetically:
// it tells you what to implement next.
//
// These are strong definitions, so they override the recompiler's weak
// aliases at link time. Replacing a stub with a real implementation means
// deleting its entry here and defining it properly elsewhere.
// ---------------------------------------------------------------------------
static int emitStubs(const Image& image, const char* outPath)
{
    std::vector<std::string> names;
    for (const auto& sym : image.symbols)
        if (sym.name.rfind("__imp__", 0) == 0)
            names.push_back(sym.name);

    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());

    if (names.empty())
    {
        fprintf(stderr, "No imports found — nothing to emit.\n");
        return EXIT_FAILURE;
    }

    std::ofstream out(outPath, std::ios::binary);
    if (!out)
    {
        fprintf(stderr, "Cannot open \"%s\" for writing.\n", outPath);
        return EXIT_FAILURE;
    }

    out << "// GENERATED by: xex_info <xex> --emit-stubs " << outPath << "\n"
        << "// Do not edit by hand — regenerate instead.\n"
        << "//\n"
        << "// One logging stub per imported kernel/OS function (" << names.size() << " total).\n"
        << "// Each records that it was called and reports the order on exit, so a run\n"
        << "// shows the game's actual boot sequence rather than a static list.\n"
        << "//\n"
        << "// Each also zeroes r3 before returning. Leaving it alone means the caller\n"
        << "// reads whatever it happened to put there as the return value, which is\n"
        << "// how a run ended up dereferencing guest 0x14 and 0xFFFFFFFD. Zero is\n"
        << "// STATUS_SUCCESS for the many NTSTATUS-returning imports, and a null\n"
        << "// handle/pointer for the rest — wrong sometimes, but wrong the same way\n"
        << "// every run, which is the difference between a bug and a mystery.\n"
        << "//\n"
        << "// These are strong definitions and override the recompiler's weak aliases.\n"
        << "//\n"
        << "// To implement one for real: write it in WoSRecomp/kernel/ and add\n"
        << "//     #define WOS_IMPL_<name> 1\n"
        << "// to WoSRecomp/kernel/kernel_overrides.h. The matching stub below then\n"
        << "// compiles out, so the two never collide and this file does not need\n"
        << "// regenerating every time a function gets implemented.\n"
        << "\n"
        << "#include \"ppc_recomp_shared.h\"\n"
        << "#include \"import_log.h\"\n"
        << "#include \"kernel_overrides.h\"\n"
        << "\n";

    for (size_t i = 0; i < names.size(); ++i)
    {
        // names[i] is "__imp__Foo"; the bare name is what the override guard
        // and the trace both use.
        const std::string bare = names[i].substr(7);

        out << "#ifndef WOS_IMPL_" << bare << "\n"
            << "PPC_FUNC(" << names[i] << ") {\n"
            << "    WOS_IMPORT_STUB(\"" << bare << "\");\n"
            << "    ctx.r3.u64 = 0;\n"
            << "}\n"
            << "#endif\n";
    }

    out << "\nsize_t WoSImportStubCount() { return " << names.size() << "; }\n";
    out.close();

    printf("Wrote %zu stub(s) to %s\n", names.size(), outPath);
    printf("Add it to the WoSRecomp target and run the host to see the boot order.\n");
    return EXIT_SUCCESS;
}

// ---------------------------------------------------------------------------
// --disasm: print the guest's own instructions.
//
// Every layer below this has been inferred: which event a thread waits on,
// what a spin loop polls, what an interrupt callback does with its arguments.
// Inference has been wrong twice. This reads the code instead.
//
// The image is already decrypted and decompressed by Image::ParseImage, and
// XenonUtils ships the same PowerPC disassembler XenonRecomp uses, so this is
// a thin wrapper: locate the section, walk instructions, print them.
//
// It prints addresses, mnemonics and operands — no data bytes, no strings —
// which is the same class of structural metadata the other modes emit.
// ---------------------------------------------------------------------------

// Branch target of a b/ba/bl/bla (op 18) or bc/bca/bcl/bcla (op 16), or 0 if
// the instruction is neither. Absolute forms ignore the current address.
static uint32_t branchTarget(uint32_t addr, uint32_t insn)
{
    if (PPC_OP(insn) == 18)
        return static_cast<uint32_t>((PPC_BA(insn) ? 0 : addr) + PPC_BI(insn));
    if (PPC_OP(insn) == 16)
        return static_cast<uint32_t>((PPC_BA(insn) ? 0 : addr) + PPC_BD(insn));
    return 0;
}

// A linking branch is a call: control comes back to the next instruction, so
// its target is somewhere else entirely and says nothing about where this
// function ends.
//
// Treating a call as an intra-function branch is not a cosmetic error. Nearly
// every function opens with `bl __savegprlr_*`, whose target is far ahead of
// the function, so "does anything branch past this terminator" was always true
// and the walk never stopped — the first run of --disasm dumped 16384
// instructions instead of the 52 the function actually has.
static bool isCallInsn(uint32_t insn)
{
    const uint32_t op = PPC_OP(insn);
    return (op == 18 || op == 16) && PPC_BL(insn);
}

// address -> symbol name, for annotating call targets. The import table is the
// interesting part: a `bl` to an __imp__ thunk names the kernel call directly.
static std::map<uint32_t, std::string> symbolMap(const Image& image)
{
    std::map<uint32_t, std::string> out;
    for (const auto& sym : image.symbols)
        out.emplace(static_cast<uint32_t>(sym.address), sym.name);
    return out;
}

static int disasm(const Image& image, uint32_t addr, uint32_t count)
{
    const Section* sec = nullptr;
    for (const auto& s : image.sections)
    {
        if (s.data != nullptr && addr >= s.base && addr < s.base + s.size)
        {
            sec = &s;
            break;
        }
    }
    if (sec == nullptr)
    {
        fprintf(stderr, "0x%08X is not inside any mapped section.\n", addr);
        return EXIT_FAILURE;
    }
    if (!(sec->flags & SectionFlags_Code))
        printf("NOTE: 0x%08X is in \"%s\", which is not marked CODE.\n\n", addr, sec->name.c_str());

    const std::map<uint32_t, std::string> symbols = symbolMap(image);

    auto readAt = [&](uint32_t a, uint32_t& insn) -> bool {
        if (a < sec->base || a + 4 > sec->base + sec->size)
            return false;
        uint32_t raw;
        std::memcpy(&raw, sec->data + (a - sec->base), 4);
        insn = ByteSwap(raw);
        return true;
    };

    // count == 0 means "run to the end of the function". A function can have
    // several `blr`s — early returns are normal — so stopping at the first one
    // truncates. Stop at a terminator only once no branch seen so far still
    // targets an address beyond it.
    const bool untilEnd = (count == 0);
    const uint32_t scanLimit = untilEnd ? 0x4000 : count;

    // The "no branch still points past here" rule is necessary but not always
    // sufficient: one forward branch to a far-away handler, or a tail call
    // emitted as a plain `b`, keeps `furthest` high and the walk never stops.
    // sub_82ACECF0 did exactly that and dumped the full 16384-instruction cap.
    //
    // .pdata is the authority on where a function ends — it is the same table
    // XenonRecomp derives its function list from — so use the record covering
    // `addr` as a hard ceiling. Note the records are per *unwind* region, so a
    // function split across consecutive records would stop early; the footer
    // says so explicitly rather than pretending the dump is complete.
    uint32_t ceiling = 0;
    const std::vector<PdataFunc> pdata = readPdata(image);
    for (const auto& f : pdata)
    {
        if (addr >= f.begin && addr < f.end)
        {
            ceiling = f.end;
            break;
        }
        // Past `addr` with nothing covering it: the next record starts the next
        // function, so that is still a valid stopping point.
        if (f.begin > addr)
        {
            ceiling = f.begin;
            break;
        }
    }
    const bool ceilingKnown = (ceiling != 0);

    // Pass 1: find the extent and collect the addresses actually branched to,
    // so pass 2 can mark exactly those. Marking "everything below the furthest
    // target" instead would flag every instruction in the range.
    uint32_t end = addr;
    std::vector<uint32_t> labels;
    {
        uint32_t furthest = addr;
        for (uint32_t i = 0; i < scanLimit; ++i)
        {
            const uint32_t a = addr + i * 4;
            uint32_t insn;
            if (!readAt(a, insn))
                break;
            end = a + 4;

            if (const uint32_t t = branchTarget(a, insn); t != 0 && !isCallInsn(insn))
            {
                furthest = std::max(furthest, t);
                labels.push_back(t);
            }
            if (untilEnd && isTerminator(insn) && a >= furthest)
                break;
            if (untilEnd && ceilingKnown && end >= ceiling)
                break;
        }
        std::sort(labels.begin(), labels.end());
        labels.erase(std::unique(labels.begin(), labels.end()), labels.end());
    }

    printf("Disassembly of 0x%08X..0x%08X in section \"%s\" (%u instructions):\n\n",
        addr, end, sec->name.c_str(), (end - addr) / 4);

    for (uint32_t a = addr; a < end; a += 4)
    {
        uint32_t insn;
        if (!readAt(a, insn))
            break;

        ppc_insn decoded{};
        ppc::Disassemble(sec->data + (a - sec->base), a, decoded);

        const bool isLabel = std::binary_search(labels.begin(), labels.end(), a);

        printf("  %08X  %08X  %c ", a, insn, isLabel ? '>' : ' ');
        if (decoded.opcode != nullptr)
            printf("%-12s %s", decoded.opcode->name, decoded.op_str);
        else
            printf("%-12s <undecodable>", ".long");

        // The disassembler already resolves branch targets into op_str, so add
        // only what it can't say: the symbol behind a call (which is how a `bl`
        // becomes "KeWaitForSingleObject"), and whether a branch goes backward
        // (which is how a spin loop becomes visible).
        if (const uint32_t t = branchTarget(a, insn); t != 0)
        {
            auto it = symbols.find(t);
            if (it != symbols.end())
                printf("   ; %s", it->second.c_str());
            else if (t <= a)
                printf("   ; backward");
        }
        printf("\n");
    }

    printf("\n'>' marks an address something in this range branches to.\n");
    if (untilEnd)
    {
        uint32_t last;
        if (readAt(end - 4, last) && isTerminator(last))
            printf("Ends on a terminator, so this is the whole function.\n");
        else if (ceilingKnown && end >= ceiling)
            printf("Stopped at the .pdata boundary 0x%08X, not on a terminator — "
                   "either the function continues in the next unwind record, or a "
                   "forward branch leaves this one.\n", ceiling);
        else
            printf("Did NOT end on a terminator — the function continues past 0x%08X.\n", end);

        if (ceilingKnown)
            printf(".pdata region for this address ends at 0x%08X.\n", ceiling);
        else
            printf("No .pdata record covers 0x%08X, so there was no hard ceiling.\n", addr);
    }

    return EXIT_SUCCESS;
}

// ---------------------------------------------------------------------------
// --xrefs: find everything that references an address.
//
// Answers the question the runtime keeps raising in reverse: not "what is this
// thread waiting for" but "who is supposed to signal it". Scans every code
// section for direct branches to the address, and every section for a stored
// pointer to it (vtables, callback tables, jump tables).
//
// Pointing this at an import thunk lists every call site of that kernel
// function — e.g. every place the game calls KeSetEvent.
// ---------------------------------------------------------------------------
// How many bytes of a section are actually backed by the decrypted image.
//
// A section's declared size can run past the end of the buffer — this XEX's
// .reloc declares 0xCCC80 but only 0x5F000 is present, which the harness
// already reports as CLAMPED when mapping. Scanning the declared size walks
// off the end of the allocation, which is what made --xrefs segfault after
// printing correct results.
static uint32_t backedSize(const Image& image, const Section& s)
{
    const uint8_t* imageBegin = image.data.get();
    const uint8_t* imageEnd = imageBegin + image.size;
    if (s.data == nullptr || s.data < imageBegin || s.data >= imageEnd)
        return 0;
    const size_t available = size_t(imageEnd - s.data);
    return (available < s.size) ? uint32_t(available) : s.size;
}

// Every D-form load/store in the image with a given displacement.
//
// --xrefs answers "who reaches this address"; it structurally cannot answer
// "who touches this field", because a struct field access encodes no address
// at all — just a base register and a 16-bit displacement. That question is
// what the D3D device stall came down to: the present call is gated on
// [device+0x2ABE] & 0x02 and the main thread's wait exits on
// [device+0x2ABD] & 0x02, and neither bit is written anywhere in the ~1600
// instructions dumped by hand.
//
// D-form instructions put the displacement in the low 16 bits and the primary
// opcode in the top 6, so the scan is exact rather than heuristic. What it
// cannot know is the base register's *type*: displacement 10942 off some
// unrelated structure looks identical. Callers get the containing function so
// they can judge; for this game the D3D code clusters in 0x82AB..0x82AD, which
// makes the real hits obvious.
static int fieldRefs(const Image& image, uint32_t displacement, bool storesOnly,
                     uint32_t context)
{
    struct Form { uint32_t op; const char* name; bool isStore; };
    // The D-form integer loads and stores. Floating-point and the DS-form
    // 64-bit pair (ld/std, primary 58/62) use the low two bits as an extension
    // rather than displacement, so they are handled separately below.
    static const Form kForms[] = {
        { 32, "lwz",  false }, { 33, "lwzu", false },
        { 34, "lbz",  false }, { 35, "lbzu", false },
        { 36, "stw",  true  }, { 37, "stwu", true  },
        { 38, "stb",  true  }, { 39, "stbu", true  },
        { 40, "lhz",  false }, { 41, "lhzu", false },
        { 42, "lha",  false }, { 43, "lhau", false },
        { 44, "sth",  true  }, { 45, "sthu", true  },
    };

    const uint16_t want = static_cast<uint16_t>(displacement);
    const std::vector<PdataFunc> pdata = readPdata(image);

    auto functionOf = [&](uint32_t a) -> uint32_t {
        for (const auto& f : pdata)
            if (a >= f.begin && a < f.end)
                return f.begin;
        return 0;
    };

    printf("Load/store instructions with displacement %u (0x%X)%s:\n\n",
        displacement, displacement, storesOnly ? ", stores only" : "");

    size_t hits = 0;
    for (const auto& s : image.sections)
    {
        if (!(s.flags & SectionFlags_Code) || s.data == nullptr)
            continue;
        const uint32_t limit = backedSize(image, s);
        for (uint32_t off = 0; off + 4 <= limit; off += 4)
        {
            uint32_t raw;
            std::memcpy(&raw, s.data + off, 4);
            const uint32_t insn = ByteSwap(raw);
            const uint32_t op = insn >> 26;

            const Form* form = nullptr;
            for (const auto& f : kForms)
                if (f.op == op) { form = &f; break; }

            if (form == nullptr)
                continue;
            if (static_cast<uint16_t>(insn & 0xFFFF) != want)
                continue;
            if (storesOnly && !form->isStore)
                continue;

            const uint32_t site = static_cast<uint32_t>(s.base) + off;
            ppc_insn decoded{};
            ppc::Disassemble(s.data + off, site, decoded);

            const uint32_t fn = functionOf(site);
            printf("  %08X  %-6s %-24s", site,
                form->isStore ? "STORE" : "load",
                decoded.opcode ? decoded.op_str : "?");
            if (fn != 0)
                printf("  in sub_%08X+0x%X", fn, site - fn);
            printf("\n");
            ++hits;

            // The address of a store says nothing about the VALUE it writes.
            // For a flags byte that is the only question worth asking — the
            // difference between `ori r9,r9,32` and `ori r9,r9,2` is the
            // difference between the field we are hunting and an unrelated
            // one, and it is invisible in the store instruction itself. So
            // print the instructions that produced the stored register and let
            // the code answer instead of the reader guessing.
            for (uint32_t k = context; k > 0; --k)
            {
                const uint32_t backOff = k * 4;
                if (backOff > off)
                    continue;
                const uint32_t prev = site - backOff;
                ppc_insn before{};
                ppc::Disassemble(s.data + (off - backOff), prev, before);
                printf("            %08X  %-10s %s\n", prev,
                    before.opcode ? before.opcode->name : ".long",
                    before.opcode ? before.op_str : "<undecodable>");
            }
            if (context > 0)
                printf("            %08X  %-10s %s   <-- the store\n\n", site,
                    decoded.opcode ? decoded.opcode->name : ".long",
                    decoded.opcode ? decoded.op_str : "<undecodable>");
        }
    }

    if (hits == 0)
        printf("  (none)\n");

    printf("\n%zu instruction(s).\n", hits);
    printf("\nNOTE: this matches the displacement only — the base register's\n"
           "type is not known, so hits against unrelated structures with the\n"
           "same offset are expected. Judge by the containing function.\n");
    return EXIT_SUCCESS;
}

static int xrefs(const Image& image, uint32_t addr)
{
    const std::map<uint32_t, std::string> symbols = symbolMap(image);
    auto nameOf = [&](uint32_t a) -> std::string {
        auto it = symbols.find(a);
        return it != symbols.end() ? it->second : std::string();
    };

    if (const std::string n = nameOf(addr); !n.empty())
        printf("Target 0x%08X is symbol \"%s\".\n\n", addr, n.c_str());
    else
        printf("Target 0x%08X (no symbol).\n\n", addr);

    size_t branches = 0, pointers = 0;

    // A call site's address is not the thing you want to look up next — the
    // *containing* function is. Reporting only the site has repeatedly cost a
    // round trip: "0x82965D90 calls it" is unactionable until you know that
    // 0x82965D90 sits inside some larger function you then have to find by
    // bisecting --disasm. .pdata already knows, so say it here.
    const std::vector<PdataFunc> pdata = readPdata(image);
    auto functionOf = [&](uint32_t a) -> uint32_t {
        for (const auto& f : pdata)
            if (a >= f.begin && a < f.end)
                return f.begin;
        return 0;
    };

    printf("--- direct branches ---\n");
    for (const auto& s : image.sections)
    {
        if (!(s.flags & SectionFlags_Code) || s.data == nullptr)
            continue;
        const uint32_t limit = backedSize(image, s);
        for (uint32_t off = 0; off + 4 <= limit; off += 4)
        {
            uint32_t raw;
            std::memcpy(&raw, s.data + off, 4);
            const uint32_t insn = ByteSwap(raw);
            const uint32_t site = static_cast<uint32_t>(s.base) + off;
            if (branchTarget(site, insn) != addr)
                continue;

            ppc_insn decoded{};
            ppc::Disassemble(s.data + off, site, decoded);
            printf("  %08X  %-10s %-24s", site,
                decoded.opcode ? decoded.opcode->name : "?", decoded.op_str);
            if (const uint32_t fn = functionOf(site); fn != 0)
                printf("  in sub_%08X+0x%X", fn, site - fn);
            else
                printf("  (no .pdata record)");
            printf("\n");
            ++branches;
        }
    }
    if (branches == 0)
        printf("  (none)\n");

    // A stored pointer is 4-byte aligned in practice (compilers align pointer
    // tables), so step by 4 rather than by 1 — the same assumption the helper
    // scan makes, and it keeps a 5 MB image scan instant.
    printf("\n--- stored pointers ---\n");
    for (const auto& s : image.sections)
    {
        if (s.data == nullptr)
            continue;
        const uint32_t limit = backedSize(image, s);
        for (uint32_t off = 0; off + 4 <= limit; off += 4)
        {
            uint32_t raw;
            std::memcpy(&raw, s.data + off, 4);
            if (ByteSwap(raw) != addr)
                continue;
            const uint32_t at = static_cast<uint32_t>(s.base) + off;
            printf("  %08X  in \"%s\"\n", at, s.name.c_str());
            ++pointers;
            if (pointers >= 64)
            {
                printf("  ... (stopping at 64)\n");
                break;
            }
        }
        if (pointers >= 64)
            break;
    }
    if (pointers == 0)
        printf("  (none)\n");

    printf("\n%zu branch(es), %zu stored pointer(s).\n", branches, pointers);
    return EXIT_SUCCESS;
}

// ---------------------------------------------------------------------------
// --func: disassemble the whole function *containing* an address.
//
// --disasm starts exactly where you point it, which is right when you already
// know a function boundary and wrong the rest of the time. Every address that
// arrives from --xrefs or from a runtime `ctx.lr` is an interior address, and
// starting a dump there hides the prologue — including the `mr rN,r3` that
// says which register the argument ended up in, which is usually the first
// thing you need. .pdata knows the enclosing region, so resolve it first.
// ---------------------------------------------------------------------------
static int disasmContaining(const Image& image, uint32_t addr)
{
    const std::vector<PdataFunc> pdata = readPdata(image);
    for (const auto& f : pdata)
    {
        if (addr >= f.begin && addr < f.end)
        {
            if (f.begin != addr)
                printf("0x%08X is inside sub_%08X (0x%08X..0x%08X), at +0x%X.\n\n",
                    addr, f.begin, f.begin, f.end, addr - f.begin);
            return disasm(image, f.begin, 0);
        }
    }

    printf("No .pdata record covers 0x%08X — disassembling from there directly.\n"
           "(Leaf functions without unwind data do not appear in .pdata, so this\n"
           "is expected for small helpers.)\n\n", addr);
    return disasm(image, addr, 0);
}

// Parse a hex-or-decimal address from the command line. Returns false rather
// than silently yielding 0, which would scan for the wrong thing.
static bool parseAddr(const char* text, uint32_t& out)
{
    char* end = nullptr;
    const unsigned long long v = std::strtoull(text, &end, 0);
    if (end == text || *end != '\0' || v > 0xFFFFFFFFull)
        return false;
    out = static_cast<uint32_t>(v);
    return true;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("Usage: xex_info <XEX/XEXP path> [--helpers] [--fix-switches <switch_tables.toml>]\n");
        printf("  (no flag)        print base address, entry point and section layout\n");
        printf("  --helpers        locate the register save/restore helper functions\n");
        printf("                   needed by WoS_config.toml (see docs/02-config-guide.md)\n");
        printf("  --fix-switches   derive `functions = [...]` overrides for switch cases\n");
        printf("                   that jump outside their detected function\n");
        printf("  --write-config   with --fix-switches, patch the functions block into\n");
        printf("                   the given config in place (keeps a .bak)\n");
        printf("  --imports        list the kernel/OS functions the game imports —\n");
        printf("                   i.e. what the runtime has to implement\n");
        printf("  --emit-stubs <f> generate logging stubs for every import, so a run\n");
        printf("                   reveals the actual boot sequence\n");
        printf("  --disasm <addr> [count]\n");
        printf("                   disassemble guest code at an address; count 0 (or\n");
        printf("                   omitted) runs to the end of the function\n");
        printf("  --field <disp> [--stores] [--context N]\n");
        printf("                   every load/store with this displacement — the\n");
        printf("                   way to find who touches a struct field, which\n");
        printf("                   --xrefs cannot do (a field access encodes no\n");
        printf("                   address, only a base register and an offset).\n");
        printf("                   --context N prints the N instructions before\n");
        printf("                   each hit, which is the only way to see what\n");
        printf("                   value a store actually writes\n");
        printf("  --func <addr>    disassemble the whole function CONTAINING an\n");
        printf("                   address. Use this for anything that came out of\n");
        printf("                   --xrefs or a runtime return address, which are\n");
        printf("                   interior addresses, not function starts\n");
        printf("  --xrefs <addr>   list every branch to, and stored pointer to, an\n");
        printf("                   address — i.e. who calls or references it. Each\n");
        printf("                   branch is annotated with its containing function\n");
        return EXIT_SUCCESS;
    }

    bool wantHelpers = false;
    bool wantImports = false;
    const char* stubsOut = nullptr;
    const char* switchToml = nullptr;
    const char* writeConfig = nullptr;
    bool wantDisasm = false, wantXrefs = false;
    bool wantField = false, fieldStoresOnly = false;
    bool wantFunc = false;
    uint32_t disasmAddr = 0, disasmCount = 0, xrefsAddr = 0, fieldDisp = 0;
    uint32_t funcAddr = 0;
    uint32_t fieldContext = 0;
    for (int i = 2; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--func") == 0)
        {
            if (i + 1 >= argc || !parseAddr(argv[i + 1], funcAddr))
            {
                fprintf(stderr, "--func requires an address, e.g. --func 0x82965D90\n");
                return EXIT_FAILURE;
            }
            ++i;
            wantFunc = true;
            continue;
        }
        if (std::strcmp(argv[i], "--disasm") == 0)
        {
            if (i + 1 >= argc || !parseAddr(argv[i + 1], disasmAddr))
            {
                fprintf(stderr, "--disasm requires an address, e.g. --disasm 0x82AC0C10\n");
                return EXIT_FAILURE;
            }
            ++i;
            wantDisasm = true;
            // An optional instruction count may follow. Only consume the next
            // argument if it actually parses as a number, so a following flag
            // isn't swallowed.
            if (i + 1 < argc && parseAddr(argv[i + 1], disasmCount))
                ++i;
            continue;
        }
        if (std::strcmp(argv[i], "--xrefs") == 0)
        {
            if (i + 1 >= argc || !parseAddr(argv[i + 1], xrefsAddr))
            {
                fprintf(stderr, "--xrefs requires an address, e.g. --xrefs 0x82AB9840\n");
                return EXIT_FAILURE;
            }
            ++i;
            wantXrefs = true;
            continue;
        }
        if (std::strcmp(argv[i], "--field") == 0)
        {
            if (i + 1 >= argc || !parseAddr(argv[i + 1], fieldDisp))
            {
                fprintf(stderr, "--field requires a displacement, e.g. --field 0x2ABE\n");
                return EXIT_FAILURE;
            }
            ++i;
            wantField = true;
            // Trailing modifiers, in any order: --stores narrows to writes,
            // --context N shows the N instructions that computed the value.
            while (i + 1 < argc)
            {
                if (std::strcmp(argv[i + 1], "--stores") == 0)
                {
                    fieldStoresOnly = true;
                    ++i;
                    continue;
                }
                if (std::strcmp(argv[i + 1], "--context") == 0)
                {
                    if (i + 2 >= argc || !parseAddr(argv[i + 2], fieldContext))
                    {
                        fprintf(stderr, "--context requires a count, e.g. --context 6\n");
                        return EXIT_FAILURE;
                    }
                    if (fieldContext > 32)
                        fieldContext = 32;
                    i += 2;
                    continue;
                }
                break;
            }
            continue;
        }
        if (std::strcmp(argv[i], "--helpers") == 0)
        {
            wantHelpers = true;
        }
        else if (std::strcmp(argv[i], "--imports") == 0)
        {
            wantImports = true;
        }
        else if (std::strcmp(argv[i], "--emit-stubs") == 0)
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "--emit-stubs requires an output path.\n");
                return EXIT_FAILURE;
            }
            stubsOut = argv[++i];
        }
        else if (std::strcmp(argv[i], "--fix-switches") == 0)
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "--fix-switches requires a path to the switch table TOML.\n");
                return EXIT_FAILURE;
            }
            switchToml = argv[++i];
        }
        else if (std::strcmp(argv[i], "--write-config") == 0)
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "--write-config requires a path to WoS_config.toml.\n");
                return EXIT_FAILURE;
            }
            writeConfig = argv[++i];
        }
    }

    const auto file = LoadFile(argv[1]);
    if (file.empty())
    {
        fprintf(stderr, "Failed to read \"%s\" (missing, empty, or unreadable).\n", argv[1]);
        return EXIT_FAILURE;
    }

    bool isXex = file.size() >= 4 && file[0] == 'X' && file[1] == 'E' && file[2] == 'X' && file[3] == '2';
    bool isElf = file.size() >= 4 && file[0] == 0x7F && file[1] == 'E' && file[2] == 'L' && file[3] == 'F';

    if (!isXex && !isElf)
    {
        fprintf(stderr, "\"%s\" does not start with an XEX2 or ELF magic — "
            "is this really a default.xex?\n", argv[1]);
        return EXIT_FAILURE;
    }

    // The upstream XEX loader trusts header-declared offsets/counts without
    // bounds-checking them against the actual file size, so a truncated or
    // corrupted dump can make it read out of bounds (crash) rather than
    // fail cleanly. Sanity-check the header ourselves first so a bad file
    // (partial extraction, wrong file, etc.) gives a message instead of a
    // segfault.
    if (isXex)
    {
        if (file.size() < sizeof(Xex2Header))
        {
            fprintf(stderr, "\"%s\" is too small to contain a valid XEX header "
                "(%zu bytes) — truncated or incomplete dump?\n", argv[1], file.size());
            return EXIT_FAILURE;
        }

        const auto* header = reinterpret_cast<const Xex2Header*>(file.data());
        const uint32_t headerSize = header->headerSize;
        const uint32_t securityOffset = header->securityOffset;
        const uint32_t headerCount = header->headerCount;

        const bool headerSizeOk = headerSize >= sizeof(Xex2Header) && headerSize <= file.size();
        const bool securityInfoOk = securityOffset >= sizeof(Xex2Header) &&
            static_cast<uint64_t>(securityOffset) + sizeof(Xex2SecurityInfo) <= file.size();
        // Each optional header entry is 8 bytes (Xex2OptHeader); the table
        // starts right after the fixed Xex2Header.
        const bool headerCountOk = static_cast<uint64_t>(sizeof(Xex2Header)) +
            static_cast<uint64_t>(headerCount) * sizeof(Xex2OptHeader) <= file.size();

        if (!headerSizeOk || !securityInfoOk || !headerCountOk)
        {
            fprintf(stderr, "\"%s\" has an XEX header that doesn't fit the file size "
                "(%zu bytes) — truncated, corrupted, or not actually a XEX. "
                "Re-check the source dump.\n", argv[1], file.size());
            return EXIT_FAILURE;
        }
    }

    Image image;
    try
    {
        image = Image::ParseImage(file.data(), file.size());
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "Failed to parse \"%s\": %s\n", argv[1], e.what());
        return EXIT_FAILURE;
    }

    if (wantFunc)
        return disasmContaining(image, funcAddr);

    if (wantDisasm)
        return disasm(image, disasmAddr, disasmCount);

    if (wantXrefs)
        return xrefs(image, xrefsAddr);

    if (wantField)
        return fieldRefs(image, fieldDisp, fieldStoresOnly, fieldContext);

    if (stubsOut != nullptr)
        return emitStubs(image, stubsOut);

    if (wantImports)
        return listImports(image);

    if (switchToml != nullptr)
        return fixSwitches(image, switchToml, writeConfig);

    if (wantHelpers)
        return findHelpers(image);

    printf("File:         %s\n", argv[1]);
    printf("File size:    0x%zX (%zu bytes)\n", file.size(), file.size());
    printf("Base address: 0x%zX\n", image.base);
    printf("Entry point:  0x%zX\n", image.entry_point);
    printf("Image size:   0x%X\n", image.size);
    printf("Sections (%zu):\n", image.sections.size());
    printf("  %-16s %-12s %-12s %s\n", "name", "base", "size", "flags");
    for (const auto& section : image.sections)
    {
        printf("  %-16s 0x%-10zX 0x%-10X %s%s\n",
            section.name.c_str(),
            section.base,
            section.size,
            (section.flags & SectionFlags_Code) ? "CODE " : "",
            (section.flags & SectionFlags_Data) ? "DATA " : "");
    }

    return EXIT_SUCCESS;
}
