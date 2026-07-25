// Prints structural metadata (base address, entry point, section layout) for
// a Xbox 360 XEX/XEXP file. Reuses XenonUtils' own loader (the same code
// XenonAnalyse/XenonRecomp use), so decryption/decompression of retail XEXs
// is handled automatically.
//
// Deliberately does NOT dump any code/data bytes or copyrighted content —
// only the structural metadata needed to start filling in the config TOML
// (see docs/02-config-guide.md).
#include <cstdio>
#include <file.h>
#include <image.h>
#include <xex.h>

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("Usage: xex_info [input XEX/XEXP file path]\n");
        return EXIT_SUCCESS;
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
