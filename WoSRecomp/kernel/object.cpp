#include "object.h"
#include "guest.h"

#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace wos
{
namespace
{

std::mutex g_mutex;
std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> g_byHandle;
std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> g_byGuestPtr;
std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> g_pseudo;

// Xbox handles are small even values with a low tag; anything non-zero and
// distinctive works. Starting well away from 0 means a handle can never be
// confused with a null/uninitialised value.
uint32_t g_nextHandle = 0x00010004;

// Each object needs a guest-visible block so it has an address the game can
// hold. 256 bytes is far more than anything reads, and keeps them on tidy
// boundaries for eyeballing in a log.
constexpr uint32_t kObjectBlockSize = 0x100;

} // namespace

uint32_t RegisterObject(uint8_t* base, const std::shared_ptr<KernelObject>& obj)
{
    const uint32_t guestPtr = GuestAlloc(base, 0, kObjectBlockSize, 0x1000);
    if (guestPtr == 0)
    {
        printf("[obj] could not allocate a guest block for a %s\n", obj->type);
        return 0;
    }

    std::lock_guard<std::mutex> lock(g_mutex);

    obj->handle = g_nextHandle;
    g_nextHandle += 4;
    obj->guestPtr = guestPtr;

    g_byHandle[obj->handle] = obj;
    g_byGuestPtr[obj->guestPtr] = obj;

    return obj->handle;
}

void RegisterObjectAt(uint32_t guestPtr, const std::shared_ptr<KernelObject>& obj)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    obj->guestPtr = guestPtr;
    g_byGuestPtr[guestPtr] = obj;
}

std::shared_ptr<KernelObject> ObjectFromHandle(uint32_t handle)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_byHandle.find(handle);
    return it == g_byHandle.end() ? nullptr : it->second;
}

std::shared_ptr<KernelObject> ObjectFromGuestPtr(uint32_t guestPtr)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_byGuestPtr.find(guestPtr);
    return it == g_byGuestPtr.end() ? nullptr : it->second;
}

std::shared_ptr<KernelObject> ObjectFromAny(uint32_t handleOrPtr)
{
    // Pseudo-handles. NT (and the 360) use these constants to mean "me"
    // without allocating anything: -1 is the current process, -2 the current
    // thread. The last run logged "unknown handle 0xFFFFFFFE" twice because
    // they were being looked up like ordinary handles and naturally not found.
    if (handleOrPtr == kCurrentProcessHandle || handleOrPtr == kCurrentThreadHandle)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_pseudo.find(handleOrPtr);
        if (it != g_pseudo.end())
            return it->second;
        // Fall through: a caller that only wants a non-null object pointer is
        // better served by one than by a failure.
    }

    if (auto obj = ObjectFromHandle(handleOrPtr))
        return obj;
    return ObjectFromGuestPtr(handleOrPtr);
}

uint32_t RegisterPseudoHandle(uint8_t* base, uint32_t pseudoHandle, const char* type)
{
    auto obj = std::make_shared<KernelObject>();
    obj->type = type;

    const uint32_t guestPtr = GuestAlloc(base, 0, kObjectBlockSize, 0x1000);
    if (guestPtr == 0)
        return 0;

    std::lock_guard<std::mutex> lock(g_mutex);
    obj->handle = pseudoHandle;
    obj->guestPtr = guestPtr;
    g_pseudo[pseudoHandle] = obj;
    g_byGuestPtr[guestPtr] = obj;
    return guestPtr;
}

void CloseHandle(uint32_t handle)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_byHandle.find(handle);
    if (it == g_byHandle.end())
        return;

    // Drop the handle mapping but keep the guest-pointer mapping: the game may
    // still hold a referenced pointer to the object, and the shared_ptr keeps
    // it alive until that goes too. Guest blocks are never reused, so a stale
    // pointer can't silently resolve to a different object.
    g_byHandle.erase(it);
}

size_t LiveObjectCount()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_byGuestPtr.size();
}

} // namespace wos
