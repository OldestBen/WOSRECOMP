#pragma once
// A window, and something in it.
//
// This is the first piece of the renderer. It deliberately does NOT translate
// PM4 or draw anything the game asked for — it opens a real window, creates a
// swapchain, and presents a frame every vblank. What it presents is whatever
// guest memory it has been pointed at, or a generated pattern if it has not
// been pointed at anything yet.
//
// WHY START HERE rather than with command translation: presentation is the
// only part of a renderer whose correctness is visible without any of the rest
// of it working. A window that updates at 60 Hz proves the frame loop, the
// vblank timing, the swapchain and the guest-memory upload path all at once,
// and every later piece can be checked by looking at it. Starting with PM4
// means writing thousands of lines before anything can be seen at all.
//
// It is also entirely independent of the loader deadlock. The game already
// drives a coherent command stream at 60 Hz; nothing about opening a window
// depends on asset loading being fixed first.

#include <cstdint>

namespace wos::gpu
{

// Open the window and initialise the swapchain. Returns false if it could not,
// having already said why — the caller carries on headless rather than
// aborting, because a failure here must never cost a diagnostic run.
bool StartPresenter(uint8_t* guestBase);

// Tell the presenter where the front buffer lives, in guest address space.
// Called from VdSwap once the game finally presents a frame. Until then the
// presenter shows a generated pattern, so the window is never blank and
// "nothing is being drawn" stays distinguishable from "the window is broken".
void SetFrontBuffer(uint32_t guestAddr, uint32_t width, uint32_t height, uint32_t pitch);

// Present one frame. Called from the vblank thread at ~60 Hz. Cheap and safe to
// call before SetFrontBuffer, and a no-op if the window failed to open.
void PresentFrame();

// True once a real front buffer has been handed over, for reporting.
bool HasFrontBuffer();

void ShutdownPresenter();

} // namespace wos::gpu
