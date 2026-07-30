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
#include <vector>
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

// FILE_INFORMATION_CLASS values the guest actually asks for.
constexpr uint32_t kFileStandardInformation   = 5;
constexpr uint32_t kFilePositionInformation   = 14;
constexpr uint32_t kFileNetworkOpenInformation = 34;

constexpr uint32_t kStatusEndOfFile           = 0xC0000011u;
constexpr uint32_t kStatusInfoLengthMismatch  = 0xC0000004u;
constexpr uint32_t kStatusInvalidInfoClass    = 0xC0000003u;

// 64-bit file offsets, spelled differently per platform.
int64_t _ftelli64_portable(FILE* fp)
{
#ifdef _WIN32
    return _ftelli64(fp);
#else
    return ftello(fp);
#endif
}

int _fseeki64_portable(FILE* fp, int64_t offset, int origin)
{
#ifdef _WIN32
    return _fseeki64(fp, offset, origin);
#else
    return fseeko(fp, off_t(offset), origin);
#endif
}

uint64_t FileSize(FILE* fp)
{
    const int64_t saved = _ftelli64_portable(fp);
    _fseeki64_portable(fp, 0, SEEK_END);
    const int64_t size = _ftelli64_portable(fp);
    _fseeki64_portable(fp, saved, SEEK_SET);
    return size < 0 ? 0 : uint64_t(size);
}

FILE* OpenRead(const char* path)
{
#ifdef _WIN32
    FILE* fp = nullptr;
    return fopen_s(&fp, path, "rb") == 0 ? fp : nullptr;
#else
    return fopen(path, "rb");
#endif
}

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

    FILE* fp = OpenRead(hostPath.string().c_str());
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

#ifdef WOS_IMPL_NtQueryInformationFile
// NTSTATUS NtQueryInformationFile(HANDLE,             // r3
//                                 PIO_STATUS_BLOCK,   // r4
//                                 PVOID FileInformation, // r5
//                                 ULONG Length,       // r6
//                                 FILE_INFORMATION_CLASS); // r7
//
// This being a stub is what broke the previous run. The guest opened
// game_shared.ini, asked for its size, got nothing written back, and read
// uninitialised guest memory as the answer — 0x82010000, which is an address,
// not a size. It then tried to allocate 2 GB for the file buffer, failed, and
// aborted. A stub that returns "success" without filling in its out-parameter
// is worse than one that returns an error.
PPC_FUNC(__imp__NtQueryInformationFile)
{
    WOS_IMPORT_STUB("NtQueryInformationFile");

    const uint32_t ioStatusBlock = ctx.r4.u32;
    const uint32_t infoOut = ctx.r5.u32;
    const uint32_t length = ctx.r6.u32;
    const uint32_t infoClass = ctx.r7.u32;

    auto obj = wos::ObjectFromAny(ctx.r3.u32);
    auto* file = dynamic_cast<wos::FileObject*>(obj.get());

    if (file == nullptr || file->fp == nullptr)
    {
        printf("[file] NtQueryInformationFile on unknown handle 0x%08X\n", ctx.r3.u32);
        ctx.r3.u64 = wos::kStatusInvalidHandle;
        return;
    }

    uint32_t written = 0;

    switch (infoClass)
    {
    case kFileStandardInformation:
    {
        if (length < 24)
        {
            ctx.r3.u64 = kStatusInfoLengthMismatch;
            return;
        }
        const uint64_t size = FileSize(file->fp);
        wos::StoreU64(base, infoOut + 0, size);   // AllocationSize
        wos::StoreU64(base, infoOut + 8, size);   // EndOfFile
        wos::StoreU32(base, infoOut + 16, 1);     // NumberOfLinks
        wos::StoreU32(base, infoOut + 20, 0);     // DeletePending / Directory
        written = 24;
        printf("[file] size of \"%s\" = %llu bytes\n",
            file->guestPath.c_str(), (unsigned long long)size);
        break;
    }

    case kFilePositionInformation:
    {
        if (length < 8)
        {
            ctx.r3.u64 = kStatusInfoLengthMismatch;
            return;
        }
        wos::StoreU64(base, infoOut + 0, uint64_t(_ftelli64_portable(file->fp)));
        written = 8;
        break;
    }

    case kFileNetworkOpenInformation:
    {
        // Used as a one-shot "does it exist and how big is it".
        if (length < 56)
        {
            ctx.r3.u64 = kStatusInfoLengthMismatch;
            return;
        }
        const uint64_t size = FileSize(file->fp);
        for (uint32_t off = 0; off < 32; off += 8)
            wos::StoreU64(base, infoOut + off, 0);   // the four timestamps
        wos::StoreU64(base, infoOut + 32, size);     // AllocationSize
        wos::StoreU64(base, infoOut + 40, size);     // EndOfFile
        wos::StoreU32(base, infoOut + 48, 0x80);     // FILE_ATTRIBUTE_NORMAL
        written = 56;
        break;
    }

    default:
        printf("[file] NtQueryInformationFile: unhandled class %u for \"%s\"\n",
            infoClass, file->guestPath.c_str());
        ctx.r3.u64 = kStatusInvalidInfoClass;
        return;
    }

    if (ioStatusBlock != 0)
    {
        wos::StoreU32(base, ioStatusBlock + 0, wos::kStatusSuccess);
        wos::StoreU32(base, ioStatusBlock + 4, written);
    }

    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

#ifdef WOS_IMPL_NtSetInformationFile
PPC_FUNC(__imp__NtSetInformationFile)
{
    WOS_IMPORT_STUB("NtSetInformationFile");

    const uint32_t ioStatusBlock = ctx.r4.u32;
    const uint32_t infoIn = ctx.r5.u32;
    const uint32_t infoClass = ctx.r7.u32;

    auto obj = wos::ObjectFromAny(ctx.r3.u32);
    auto* file = dynamic_cast<wos::FileObject*>(obj.get());

    if (file == nullptr || file->fp == nullptr)
    {
        ctx.r3.u64 = wos::kStatusInvalidHandle;
        return;
    }

    if (infoClass == kFilePositionInformation)
    {
        const uint64_t pos = wos::LoadU64(base, infoIn);
        _fseeki64_portable(file->fp, int64_t(pos), SEEK_SET);
    }
    else
    {
        printf("[file] NtSetInformationFile: unhandled class %u\n", infoClass);
    }

    if (ioStatusBlock != 0)
    {
        wos::StoreU32(base, ioStatusBlock + 0, wos::kStatusSuccess);
        wos::StoreU32(base, ioStatusBlock + 4, 0);
    }

    ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

namespace wos { void ReportRequestBlock(uint8_t* base, const char* when); }

#ifdef WOS_IMPL_NtReadFile
// NTSTATUS NtReadFile(HANDLE FileHandle,     // r3
//                     HANDLE Event,          // r4
//                     PIO_APC_ROUTINE,       // r5
//                     PVOID ApcContext,      // r6
//                     PIO_STATUS_BLOCK,      // r7
//                     PVOID Buffer,          // r8
//                     ULONG Length,          // r9
//                     PLARGE_INTEGER ByteOffset); // r10
PPC_FUNC(__imp__NtReadFile)
{
    WOS_IMPORT_STUB("NtReadFile");

    const uint32_t eventHandle = ctx.r4.u32;
    const uint32_t apcRoutine = ctx.r5.u32;
    const uint32_t apcContext = ctx.r6.u32;
    const uint32_t ioStatusBlock = ctx.r7.u32;
    const uint32_t buffer = ctx.r8.u32;
    const uint32_t length = ctx.r9.u32;
    const uint32_t byteOffsetPtr = ctx.r10.u32;

    auto obj = wos::ObjectFromAny(ctx.r3.u32);
    auto* file = dynamic_cast<wos::FileObject*>(obj.get());

    if (file == nullptr || file->fp == nullptr)
    {
        printf("[file] NtReadFile on unknown handle 0x%08X\n", ctx.r3.u32);
        ctx.r3.u64 = wos::kStatusInvalidHandle;
        return;
    }

    // A ByteOffset of -1 (or -2) means "use the current position"; anything
    // else is an absolute seek for this read only.
    if (byteOffsetPtr != 0)
    {
        const int64_t offset = int64_t(wos::LoadU64(base, byteOffsetPtr));
        if (offset >= 0)
            _fseeki64_portable(file->fp, offset, SEEK_SET);
    }

    const size_t read = fread(base + buffer, 1, length, file->fp);

    if (ioStatusBlock != 0)
    {
        wos::StoreU32(base, ioStatusBlock + 0,
            read == 0 && length != 0 ? kStatusEndOfFile : wos::kStatusSuccess);
        wos::StoreU32(base, ioStatusBlock + 4, uint32_t(read));
    }

    // Asynchronous reads signal the event on completion. Ours are synchronous,
    // so it is already complete by the time we get here — signal immediately
    // rather than leaving a waiter stuck forever.
    if (eventHandle != 0)
        wos::SignalEventIfAny(eventHandle);

    // Queue the completion APC for delivery at the next wait on this thread.
    //
    // Delivering it inline here does not work, and the reason is instructive.
    // The trampoline at guest 0x82B16658 does:
    //
    //     mr   r31,r4        ; IoStatusBlock
    //     mr   r30,r3        ; ApcContext
    //     lwz  r3,0(r31)     ; IOSB.Status
    //     lwz  r4,4(r31)     ; IOSB.Information
    //     mtctr r30 / bctrl  ; call ApcContext(error, bytes, iosb)
    //
    // and the context it calls, 0x829688C0, immediately reads [0x82F71AE4] —
    // a field of the same request block the Game Master takes its wait handle
    // from. On the real console NtReadFile returns first and the caller
    // finishes populating that block; the APC fires afterwards. Called inline,
    // the APC reads the block before the caller has written it.
    //
    // The real kernel delivers an APC to the ISSUING THREAD when it next
    // enters an alertable wait, which is both the correct semantics and the
    // simplest thing that fixes the ordering: queue it here, drain it at the
    // top of the wait implementations. Thread-local, so it lands on the thread
    // that issued the read, which is what the guest's own thread-affine state
    // expects.
    if (apcRoutine != 0)
    {
        wos::ReportRequestBlock(base, "at queue");
        wos::QueueThreadApc(apcRoutine, apcContext, ioStatusBlock);
    }

    printf("[file] read %zu of 0x%X bytes from \"%s\" into guest 0x%08X\n",
        read, length, file->guestPath.c_str(), buffer);

    // An async read — one carrying a completion APC — returns STATUS_PENDING on
    // the real kernel, and the completion arrives later. We were returning
    // SUCCESS, which tells the caller the transfer finished synchronously and
    // that no APC is coming.
    //
    // That is a candidate for why delivering the APC changes nothing. The
    // completion at guest 0x82965488 signals only when the request object is in
    // state 1; states 2 and 3 return silently. A caller told "already done"
    // would reasonably advance that state itself, and our APC then arrives to
    // find a request it is no longer allowed to complete — which looks exactly
    // like the APC doing nothing.
    if (read == 0 && length != 0)
        ctx.r3.u64 = kStatusEndOfFile;
    else if (apcRoutine != 0)
        ctx.r3.u64 = wos::kStatusPending;
    else
        ctx.r3.u64 = wos::kStatusSuccess;
}
#endif

// ---------------------------------------------------------------------------
// Per-thread APC queue.
//
// See the comment at the queue site in NtReadFile for why these cannot be
// delivered inline. Kept here rather than in sync.cpp because the only
// producer is the file layer; sync.cpp merely drains it.
// ---------------------------------------------------------------------------

namespace
{

struct PendingApc
{
    uint32_t routine = 0;   // raw, low bits still set
    uint32_t context = 0;
    uint32_t iosb = 0;
};

// Thread-local by design: an APC belongs to the thread that issued the I/O,
// and guest completion routines touch thread-affine state.
thread_local std::vector<PendingApc> t_apcQueue;

} // namespace

namespace wos
{

// The request block the completion chain walks.
//
// 0x829688C0 — the ApcContext the trampoline calls — does:
//     addi r31,r11,6860   ; r31 = 0x82F71ACC
//     lwz  r3,24(r31)     ; [+0x18]
//     bl   0x82968498     ; the real completion
//     stw  r11,16(r31)    ; [+0x10] = 0
//
// and the Game Master takes its first wait handle from [+0x14] of the same
// block. So these three fields are the whole handshake, and whether they are
// populated when the APC runs is the question that decides whether the
// ordering is right.
void ReportRequestBlock(uint8_t* base, const char* when)
{
    constexpr uint32_t kBlock = 0x82F71ACC;
    static unsigned s_logged = 0;
    if (s_logged >= 8)
        return;
    ++s_logged;
    printf("[file] request block %s: [+0x10]=0x%08X [+0x14]=0x%08X [+0x18]=0x%08X\n",
        when,
        LoadU32(base, kBlock + 0x10),
        LoadU32(base, kBlock + 0x14),
        LoadU32(base, kBlock + 0x18));
}

void QueueThreadApc(uint32_t routine, uint32_t context, uint32_t iosb)
{
    t_apcQueue.push_back({ routine, context, iosb });
}

// ---------------------------------------------------------------------------
// AN EXPERIMENT, NOT A FIX. Off unless WOS_PROBE_COMPLETE_IO is set.
//
// The diagnosis says the async read completes correctly and nothing consumes
// the result. Specifically, after the APC runs:
//
//   - the file request at [0x82F71ACC+0x18] is in state 2 (completed OK) and
//     carries the async-request handle 0x00000080 at its +0x34;
//   - request object [0] is in state 1, which is the first branch
//     sub_82965488 takes, straight to the signaller;
//   - that handle passes every validation step in sub_82965488 — checked by
//     hand against the live values: 0x80 & 0x7F = slot 0, 0 < count 64,
//     [table + 0*112 + 8] = 0x80 which matches, index 0, object 0x402352C0;
//   - the signaller then sets the handle at [0x82F719DC] = 0x00010030 = ev4,
//     which is one of the two events the Game Master is blocked on.
//
// Every link is verified except the call itself. This makes the call, and so
// tests the whole chain in one run rather than by reading four more functions.
//
// It is deliberately NOT the default. If the game springs to life with this
// set, the diagnosis is confirmed and the real work is finding what makes that
// call on the console. If nothing changes, the diagnosis is wrong somewhere
// and the reading has to continue — which is worth knowing just as much.
void ProbeCompleteAsyncRequest(PPCContext& ctx, uint8_t* base)
{
    static const bool s_enabled = std::getenv("WOS_PROBE_COMPLETE_IO") != nullptr;
    if (!s_enabled)
        return;

    constexpr uint32_t kRequestBlock = 0x82F71ACC;
    constexpr uint32_t kComplete     = 0x82965488;

    const uint32_t fileRequest = LoadU32(base, kRequestBlock + 0x18);
    if (fileRequest < 0x82000000u || fileRequest >= 0x83000000u)
    {
        printf("[probe] file request 0x%08X is not an image address — skipped\n",
            fileRequest);
        return;
    }

    // The handle the file request carries, at the offset the live dump found
    // it at. The high bit marks a free slot, and sub_82965488 rejects those
    // outright, so refuse rather than call it with something it will discard
    // silently — a silent rejection would look exactly like the probe having
    // no effect, which is the one outcome that must stay distinguishable.
    const uint32_t handle = LoadU32(base, fileRequest + 0x34);
    if (handle == 0 || (handle & 0x80000000u) != 0)
    {
        printf("[probe] request handle [0x%08X+0x34] = 0x%08X is not a live "
               "handle — not calling\n", fileRequest, handle);
        return;
    }

    PPCFunc* complete = PPC_LOOKUP_FUNC(base, kComplete);
    if (complete == nullptr)
    {
        printf("[probe] no recompiled function at 0x%08X\n", kComplete);
        return;
    }

    printf("[probe] calling sub_%08X(0x%08X) to complete the async request\n",
        kComplete, handle);

    const PPCContext saved = ctx;
    ctx.r3.u64 = handle;
    complete(ctx, base);
    ctx = saved;

    printf("[probe] returned; request object state is now %u\n",
        LoadU32(base, LoadU32(base, 0x82F71A64) + 0x34));
}

void DeliverPendingApcs(PPCContext& ctx, uint8_t* base)
{
    if (t_apcQueue.empty())
        return;

    // Move first: a completion routine may issue another read and queue a
    // further APC, and appending to the vector being iterated would be a
    // use-after-realloc. The new one is delivered at the next wait.
    std::vector<PendingApc> queue;
    queue.swap(t_apcQueue);

    for (const auto& apc : queue)
    {
        // The routine address arrives with its low bits set (0x82B16659).
        // PowerPC instructions are 4-byte aligned, so those are flags.
        const uint32_t target = apc.routine & ~3u;
        PPCFunc* routine = PPC_LOOKUP_FUNC(base, target);
        if (routine == nullptr)
        {
            printf("[file] queued APC 0x%08X (masked 0x%08X) is not a known "
                   "function — dropped\n", apc.routine, target);
            continue;
        }

        static unsigned s_logged = 0;
        if (s_logged < 4)
        {
            ++s_logged;
            printf("[file] delivering queued APC 0x%08X -> 0x%08X, context 0x%08X\n",
                apc.routine, target, apc.context);
        }

        ReportRequestBlock(base, "at delivery");

        const PPCContext saved = ctx;
        ctx.r3.u64 = apc.context;
        ctx.r4.u64 = apc.iosb;
        ctx.r5.u64 = 0;
        routine(ctx, base);
        ctx = saved;

        ProbeCompleteAsyncRequest(ctx, base);
    }
}

} // namespace wos
