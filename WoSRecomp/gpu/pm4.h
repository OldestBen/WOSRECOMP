#pragma once
// PM4 command stream: names, register file, and a draw census.
//
// The command processor in kernel/video.cpp already walks the ring correctly —
// it handles the type-2 filler, executes type-0 register writes, follows
// indirect buffers and performs the memory-write packets the GPU fence rides
// on. What it cannot do is tell you what the game is actually ASKING FOR,
// because every packet is an unnamed opcode number and every register is an
// unnamed index.
//
// That gap is now the main cost of renderer work. Translating PM4 into draw
// calls means knowing which packets are draws, which registers hold the vertex
// format, the shader addresses, the render target and the viewport — and every
// one of those questions currently requires cross-referencing an opcode number
// against notes. Naming them once, here, turns the existing log into a
// specification of what the renderer has to implement.
//
// This is deliberately NOT a translator. It decodes, names, and counts. The
// first thing a renderer needs is an accurate list of what it must handle, and
// the honest way to get that is to read it off the game rather than guess from
// hardware documentation about which subset a particular title uses.

#include <cstdint>

namespace wos::gpu
{

// Human name for a type-3 opcode, or nullptr if it is not one we know.
const char* Pm4OpcodeName(uint32_t opcode);

// Human name for a register index, or nullptr.
const char* Pm4RegisterName(uint32_t reg);

// True if this type-3 opcode issues geometry. These are the packets a
// translator has to turn into host draw calls; everything else is state.
bool Pm4IsDraw(uint32_t opcode);

// Record a type-3 packet. Counts it, and for draws captures the register state
// that was live at the time so the report can say what a draw actually needs.
void Pm4RecordPacket(uint32_t opcode, uint32_t count);

// Record a register write, so the report can distinguish registers the game
// actually uses from the hundreds it never touches.
void Pm4RecordRegister(uint32_t reg, uint32_t value);

// Print what the stream contains: every opcode seen with its count and name,
// every register written with its last value, and the draw total. Called from
// the heartbeat.
//
// This is the renderer's to-do list, derived from the game rather than from
// documentation.
void Pm4Report();

} // namespace wos::gpu
