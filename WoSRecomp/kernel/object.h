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

// Pseudo-handles: NT constants meaning "me", needing no allocation.
constexpr uint32_t kCurrentProcessHandle = 0xFFFFFFFFu;
constexpr uint32_t kCurrentThreadHandle  = 0xFFFFFFFEu;

// Accepts a handle *or* a guest pointer *or* a pseudo-handle, because the
// guest mixes all three freely.
std::shared_ptr<KernelObject> ObjectFromAny(uint32_t handleOrPtr);

// Give a pseudo-handle a real backing object so ObReferenceObjectByHandle can
// return a usable pointer for it. Returns the guest pointer, or 0.
uint32_t RegisterPseudoHandle(uint8_t* base, uint32_t pseudoHandle, const char* type);

// Register an object at a guest address the *guest* chose, rather than one we
// allocated. Needed for dispatcher objects the game declares inline in its own
// memory (a KEVENT in a struct, say) and initialises in place, so they never
// pass through NtCreateEvent and we never see them created.
void RegisterObjectAt(uint32_t guestPtr, const std::shared_ptr<KernelObject>& obj);

void CloseHandle(uint32_t handle);

// How many objects are live, for the run summary.
size_t LiveObjectCount();

// Signal an event if the value names one; does nothing otherwise.
// Used by the synchronous file reads to complete their "async" event.
void SignalEventIfAny(uint32_t handleOrPtr);

// Print per-event wait statistics: waits / timeouts / signals. An event with
// waits ~= timeouts and zero signals is one nothing ever wakes, which names a
// missing piece of the runtime precisely.
void ReportWaitActivity();

// Wait on a thread handle — "block until this thread exits", which is what
// waiting on a thread object means and what we previously could not express.
// ThreadObject is defined inside thread.cpp, so sync.cpp cannot dynamic_cast to
// it; this is the seam instead.
//
// Returns:
//    1  the handle named a thread and it has exited
//    0  the handle named a thread and the wait timed out
//   -1  the handle did not name a thread — the caller should keep looking
//
// timeoutMs < 0 is INFINITE, matching the wait imports.
int WaitForThreadExit(uint32_t handleOrPtr, int64_t timeoutMs);

} // namespace wos
