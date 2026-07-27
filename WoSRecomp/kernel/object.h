#pragma once
// Kernel objects and handles.
//
// The guest refers to kernel objects two different ways and expects both to
// work interchangeably:
//
//   * by HANDLE  — an opaque 32-bit value from NtCreateEvent, ExCreateThread…
//   * by POINTER — a guest address, which ObReferenceObjectByHandle hands back
//                  and ObDereferenceObject/KeWaitForSingleObject then take
//
// So every object gets registered twice: once under its handle, and once under
// a small guest-visible block whose address serves as the pointer. The block's
// contents are never read by us — only its address matters — but it must be
// real guest memory, because the game will happily store it, compare it, and
// pass it back.

#include <cstdint>
#include <memory>
#include <string>

namespace wos
{

struct KernelObject
{
    virtual ~KernelObject() = default;

    uint32_t handle = 0;      // guest HANDLE
    uint32_t guestPtr = 0;    // guest address used as the object pointer
    std::string name;         // for diagnostics only
    const char* type = "object";
};

// Registers an object, assigning it a handle and a guest pointer.
// Returns the handle, or 0 if guest memory could not be allocated.
uint32_t RegisterObject(uint8_t* base, const std::shared_ptr<KernelObject>& obj);

// Either lookup returns nullptr if the value does not name a live object.
std::shared_ptr<KernelObject> ObjectFromHandle(uint32_t handle);
std::shared_ptr<KernelObject> ObjectFromGuestPtr(uint32_t guestPtr);

// Accepts a handle *or* a guest pointer, because the guest mixes them freely.
std::shared_ptr<KernelObject> ObjectFromAny(uint32_t handleOrPtr);

void CloseHandle(uint32_t handle);

// How many objects are live, for the run summary.
size_t LiveObjectCount();

// Signal an event if the value names one; does nothing otherwise.
// Used by the synchronous file reads to complete their "async" event.
void SignalEventIfAny(uint32_t handleOrPtr);

} // namespace wos
