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
#include <utility>
#include <vector>

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

// Record which guest instruction called an import, and with what argument.
//
// The heartbeat answers "which import is hot"; it cannot answer "which line of
// game code is calling it", and for a spin those are different questions with
// different fixes. XenonRecomp writes the guest return address into ctx.lr
// immediately before every `bl`, so an implementation can name its own call
// site for free — the same trick the wait census uses, generalised so any hot
// import can be attributed without building a bespoke probe each time.
//
// `detail` is whatever single number makes the call legible: a sleep duration,
// a handle, a size. Bounded internally; recording is cheap enough to sit in a
// path called millions of times a second.
void LogCallSite(const char* name, uint32_t callSite, uint32_t detail);

// Print the recorded call sites, busiest first. Called from the heartbeat.
void ReportCallSites();

// A snapshot of call counts, for the periodic heartbeat. Returns pairs of
// (name, total calls so far), so a caller can diff two snapshots and see what
// the game is actually busy doing rather than only what it touched first.
std::vector<std::pair<const char*, uint64_t>> SnapshotImportCounts();

} // namespace wos

#define WOS_IMPORT_STUB(name) ::wos::LogImportCall(name)

// Defined by the generated file so the host can report coverage.
size_t WoSImportStubCount();
