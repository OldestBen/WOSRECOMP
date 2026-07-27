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
    if (auto obj = ObjectFromHandle(handleOrPtr))
        return obj;
    return ObjectFromGuestPtr(handleOrPtr);
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
