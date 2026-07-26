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
#include <algorithm>
#include <file.h>
#include <image.h>
#include <xex.h>
#include <xbox.h>
#include <byteswap.h>
#include <toml++/toml.hpp>
#include <ppc.h>

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

static int fixSwitches(const Image& image, const char* switchTomlPath)
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

            uint32_t end = std::max(maxTarget + 4u, site + 4u);
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
    printf("Each entry spans from the function's start to its furthest switch\n");
    printf("target. Starts inside .pdata come from the unwind record; starts in\n");
    printf("the gaps between records are recovered by scanning back to the\n");
    printf("previous function's terminator, clamped so they can never overlap a\n");
    printf("known record. Re-run XenonRecomp afterwards; config entries are\n");
    printf("registered before .pdata, so these take priority.\n");

    return EXIT_SUCCESS;
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
        return EXIT_SUCCESS;
    }

    bool wantHelpers = false;
    const char* switchToml = nullptr;
    for (int i = 2; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--helpers") == 0)
        {
            wantHelpers = true;
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

    if (switchToml != nullptr)
        return fixSwitches(image, switchToml);

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
