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
// Strings — kernel/rtl.cpp
// ---------------------------------------------------------------------------
#define WOS_IMPL_RtlInitAnsiString 1
