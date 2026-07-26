#pragma once
// Call tracing for unimplemented imports.
//
// Every generated stub in imports_generated.cpp calls WOS_IMPORT_STUB with its
// own name. That turns a run into a trace of the game's real boot sequence:
// which kernel functions it reaches, in what order, and how often.
//
// This matters because unimplemented imports fail *silently* — XenonUtils
// rewrites each import thunk to nop/nop/nop/blr, so a missing function returns
// success-shaped garbage rather than announcing itself. Without tracing, the
// game just misbehaves for no visible reason.

#include <cstddef>
#include <cstdint>

namespace wos
{

// Record a call. First call for a given name prints it; repeats are counted.
void LogImportCall(const char* name);

// Print the trace: call order, then totals. Safe to call more than once.
void DumpImportLog();

// Same, but for use from a crash handler. Takes the lock only if it is free:
// if the fault happened inside LogImportCall the lock is already held by this
// same thread, and blocking on it would deadlock — turning a crash report
// into a hang, which is strictly worse than a slightly racy one.
void DumpImportLogUnsafe();

} // namespace wos

#define WOS_IMPORT_STUB(name) ::wos::LogImportCall(name)

// Defined by the generated file so the host can report coverage.
size_t WoSImportStubCount();
