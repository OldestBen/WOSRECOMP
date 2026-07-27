#pragma once
// Which imports are implemented for real, rather than logged and ignored.
//
// kernel/imports_generated.cpp wraps every stub in `#ifndef WOS_IMPL_<name>`,
// so defining a name here compiles its stub out and leaves our own definition
// as the only one. Add the #define at the same time as the implementation —
// forgetting it produces a duplicate symbol at link time, which is a loud and
// immediate failure rather than a silent wrong one.
//
// Keep this list grouped and commented. It doubles as the honest answer to
// "how much of the kernel actually exists yet?"

// ---------------------------------------------------------------------------
// Panic / shutdown — kernel/panic.cpp
//
// These are DECLSPEC_NORETURN on the real console. Stubbing them as ordinary
// returning functions is what turned the game's first failed assertion into
// ~19,000 nested bugchecks and a runaway stack.
// ---------------------------------------------------------------------------
#define WOS_IMPL_KeBugCheck 1
#define WOS_IMPL_KeBugCheckEx 1
#define WOS_IMPL_HalReturnToFirmware 1

// ---------------------------------------------------------------------------
// Virtual memory — kernel/memory.cpp
// ---------------------------------------------------------------------------
#define WOS_IMPL_NtAllocateVirtualMemory 1
#define WOS_IMPL_NtFreeVirtualMemory 1

// ---------------------------------------------------------------------------
// Kernel objects, events, mutants — kernel/sync.cpp
//
// The game created five events, six threads and a mutant before giving up.
// With stubs these all returned garbage handles, so every operation on them
// afterwards was nonsense — and it raised exactly one exception per thread.
// ---------------------------------------------------------------------------
#define WOS_IMPL_NtCreateEvent 1
#define WOS_IMPL_NtSetEvent 1
#define WOS_IMPL_NtClearEvent 1
#define WOS_IMPL_NtPulseEvent 1
#define WOS_IMPL_NtCreateMutant 1
#define WOS_IMPL_NtReleaseMutant 1
#define WOS_IMPL_NtWaitForSingleObjectEx 1
#define WOS_IMPL_NtClose 1
#define WOS_IMPL_ObReferenceObjectByHandle 1
#define WOS_IMPL_ObDereferenceObject 1

// ---------------------------------------------------------------------------
// Threads — kernel/thread.cpp
//
// Guest threads become real host threads: recompiled functions are ordinary
// C++ functions, so a fresh PPCContext and a fresh guest stack is all one
// needs. Affinity and priority are accepted and ignored.
// ---------------------------------------------------------------------------
#define WOS_IMPL_ExCreateThread 1
#define WOS_IMPL_NtResumeThread 1
#define WOS_IMPL_NtSuspendThread 1
#define WOS_IMPL_KeSetAffinityThread 1
#define WOS_IMPL_KeSetBasePriorityThread 1
#define WOS_IMPL_KeQueryBasePriorityThread 1

// ---------------------------------------------------------------------------
// Files — kernel/file.cpp
// ---------------------------------------------------------------------------
#define WOS_IMPL_NtCreateFile 1
#define WOS_IMPL_NtOpenFile 1
// NtQueryInformationFile being a stub is what broke the previous run: the game
// asked game_shared.ini its size, got nothing written back, read uninitialised
// memory as the answer (0x82010000 — an address, not a size) and tried to
// allocate 2 GB for the buffer.
#define WOS_IMPL_NtQueryInformationFile 1
#define WOS_IMPL_NtSetInformationFile 1
#define WOS_IMPL_NtReadFile 1

// ---------------------------------------------------------------------------
// Thread-local storage — kernel/thread.cpp
// ---------------------------------------------------------------------------
#define WOS_IMPL_KeTlsAlloc 1
#define WOS_IMPL_KeTlsFree 1
#define WOS_IMPL_KeTlsGetValue 1
#define WOS_IMPL_KeTlsSetValue 1

// ---------------------------------------------------------------------------
// Critical sections — kernel/thread.cpp
// ---------------------------------------------------------------------------
#define WOS_IMPL_RtlInitializeCriticalSection 1
#define WOS_IMPL_RtlInitializeCriticalSectionAndSpinCount 1
#define WOS_IMPL_RtlEnterCriticalSection 1
#define WOS_IMPL_RtlLeaveCriticalSection 1
#define WOS_IMPL_RtlDeleteCriticalSection 1

// ---------------------------------------------------------------------------
// Strings and exceptions — kernel/rtl.cpp
// ---------------------------------------------------------------------------
#define WOS_IMPL_RtlInitAnsiString 1
// Not an error path: 0x406D1388 is the SetThreadName convention, which is why
// there was exactly one raise per thread created.
#define WOS_IMPL_RtlRaiseException 1
