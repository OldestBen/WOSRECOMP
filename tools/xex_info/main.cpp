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
#include <file.h>
#include <image.h>
#include <xex.h>

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

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("Usage: xex_info [input XEX/XEXP file path] [--helpers]\n");
        printf("  (no flag)  print base address, entry point and section layout\n");
        printf("  --helpers  locate the register save/restore helper functions\n");
        printf("             needed by WoS_config.toml (see docs/02-config-guide.md)\n");
        return EXIT_SUCCESS;
    }

    bool wantHelpers = false;
    for (int i = 2; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--helpers") == 0)
            wantHelpers = true;
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
