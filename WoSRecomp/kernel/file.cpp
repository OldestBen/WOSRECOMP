// File I/O.
//
// Deliberately minimal: this resolves and *reports* the paths the game asks
// for, and opens them if a game data directory has been pointed at. Knowing
// which paths it wants is worth more right now than a complete filesystem,
// because the answer determines how the data directory has to be laid out.
//
// Point WOS_GAME_ROOT at your extracted disc, or drop it in private/game/.

#include "ppc_recomp_shared.h"
#include "import_log.h"
#include "kernel_overrides.h"
#include "guest.h"
#include "object.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace wos
{

struct FileObject : KernelObject
{
    FILE* fp = nullptr;
    std::string guestPath;
    std::string hostPath;

    FileObject() { type = "file"; }
    ~FileObject() override { if (fp != nullptr) fclose(fp); }
};

} // namespace wos

namespace
{

std::once_flag g_rootOnce;
std::filesystem::path g_gameRoot;

const std::filesystem::path& GameRoot()
{
    std::call_once(g_rootOnce, []
    {
#ifdef _WIN32
        char buf[MAX_PATH];
        const DWORD n = GetEnvironmentVariableA("WOS_GAME_ROOT", buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf))
            g_gameRoot = buf;
#else
        if (const char* env = getenv("WOS_GAME_ROOT"))
            g_gameRoot = env;
#endif
        if (g_gameRoot.empty() && std::filesystem::is_directory("private/game"))
            g_gameRoot = "private/game";

        if (g_gameRoot.empty())
        {
            printf("[file] no game data directory. Set WOS_GAME_ROOT or create\n"
                   "       private/game/ from your extracted disc. Opens will fail,\n"
                   "       but the paths requested are still reported below.\n");
        }
        else
        {
            printf("[file] game root: %s\n", g_gameRoot.string().c_str());
        }
    });
    return g_gameRoot;
}

// Read an OBJECT_ATTRIBUTES and pull out the object name.
//
// Xbox 360 OBJECT_ATTRIBUTES is { PVOID RootDirectory; PANSI_STRING ObjectName;
// ULONG Attributes; } — note it holds a *pointer* to an ANSI_STRING, not one
// inline, which differs from desktop NT's UNICODE_STRING layout.
std::string ReadObjectName(uint8_t* base, uint32_t objectAttributes)
{
    if (objectAttributes == 0)
        return {};

    const uint32_t namePtr = wos::LoadU32(base, objectAttributes + 4);
    if (namePtr == 0)
        return {};

    const uint16_t length = wos::LoadU16(base, namePtr + 0);
    const uint32_t buffer = wos::LoadU32(base, namePtr + 4);
    if (buffer == 0 || length == 0)
        return {};

    return std::string(wos::GuestPtr(base, buffer), length);
}

// Map a guest path onto the host.
//
// The game uses device paths like "\Device\Harddisk0\Partition1\..." or
// drive-style "game:\...". Everything before the first useful separator is
// device naming we don't model, so it is stripped and the remainder treated as
// relative to the game root.
std::filesystem::path ResolveGuestPath(const std::string& guestPath)
{
    if (GameRoot().empty())
        return {};

    std::string rel = guestPath;

    if (const size_t colon = rel.find(':'); colon != std::string::npos)
    {
        rel = rel.substr(colon + 1);                      // "game:\foo" -> "\foo"
    }
    else if (rel.rfind("\\Device\\", 0) == 0)
    {
        // Skip \Device\<name>\<partition>\ — four backslashes in.
        size_t pos = 0;
        for (int i = 0; i < 4 && pos != std::string::npos; ++i)
            pos = rel.find('\\', pos + 1);
        rel = (pos == std::string::npos) ? std::string{} : rel.substr(pos + 1);
    }

    while (!rel.empty() && (rel.front() == '\\' || rel.front() == '/'))
        rel.erase(rel.begin());

    std::replace(rel.begin(), rel.end(), '\\', '/');

    if (rel.empty())
        return {};

    return GameRoot() / rel;
}

} // namespace

#ifdef WOS_IMPL_NtCreateFile
// NTSTATUS NtCreateFile(PHANDLE FileHandle,          // r3
//                       ACCESS_MASK DesiredAccess,   // r4
//                       POBJECT_ATTRIBUTES,          // r5
//                       PIO_STATUS_BLOCK,            // r6
//                       PLARGE_INTEGER AllocationSize, // r7
//                       ULONG FileAttributes,        // r8
//                       ULONG ShareAccess,           // r9
//                       ULONG CreateDisposition,     // r10
//                       ULONG CreateOptions);        // stack
PPC_FUNC(__imp__NtCreateFile)
{
    WOS_IMPORT_STUB("NtCreateFile");

    const uint32_t handleOut = ctx.r3.u32;
    const uint32_t objectAttributes = ctx.r5.u32;
    const uint32_t ioStatusBlock = ctx.r6.u32;

    const std::string guestPath = ReadObjectName(base, objectAttributes);
    const std::filesystem::path hostPath = ResolveGuestPath(guestPath);

    auto fail = [&](uint32_t status, const char* why)
    {
        printf("[file] open FAILED \"%s\" (%s)\n",
            guestPath.empty() ? "<no name>" : guestPath.c_str(), why);
        if (ioStatusBlock != 0)
        {
            wos::StoreU32(base, ioStatusBlock + 0, status);
            wos::StoreU32(base, ioStatusBlock + 4, 0);
        }
        ctx.r3.u64 = status;
    };

    if (guestPath.empty())
        return fail(wos::kStatusObjectPathNotFound, "could not read the object name");

    if (hostPath.empty())
        return fail(wos::kStatusNoSuchFile, "no game root configured");

    FILE* fp = fopen(hostPath.string().c_str(), "rb");
    if (fp == nullptr)
        return fail(wos::kStatusNoSuchFile, "not found on the host");

    auto file = std::make_shared<wos::FileObject>();
    file->fp = fp;
    file->guestPath = guestPath;
    file->hostPath = hostPath.string();
    file->name = guestPath;

    const uint32_t handle = wos::RegisterObject(base, file);
    if (handle == 0)
        return fail(wos::kStatusNoMemory, "no guest memory for the handle");

    if (handleOut != 0)
        wos::StoreU32(base, handleOut, handle);
    if (ioStatusBlock != 0)
    {
        wos::StoreU32(base, ioStatusBlock + 0, wos::kStatusSuccess);
        wos::StoreU32(base, ioStatusBlock + 4, 1);   // FILE_OPENED
    }

    printf("[file] opened \"%s\" -> %s\n", guestPath.c_str(), file->hostPath.c_str());
    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

#ifdef WOS_IMPL_NtOpenFile
PPC_FUNC(__imp__NtOpenFile)
{
    WOS_IMPORT_STUB("NtOpenFile");
    // Same shape as NtCreateFile minus the creation arguments; the guest uses
    // it for read-only opens. Report the path so it shows up in the trace.
    const std::string guestPath = ReadObjectName(base, ctx.r5.u32);
    printf("[file] NtOpenFile \"%s\" — not implemented\n",
        guestPath.empty() ? "<no name>" : guestPath.c_str());
    ctx.r3.u64 = wos::kStatusNoSuchFile;
}
#endif
