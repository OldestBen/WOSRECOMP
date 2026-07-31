// See pm4.h. Names and counts, not translation.

#include "pm4.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <vector>

namespace wos::gpu
{
namespace
{

struct Named { uint32_t id; const char* name; };

// Type-3 opcodes. Only the ones that matter to a title are listed; the
// register-access and debug packets the kernel uses are omitted deliberately,
// because an exhaustive table would bury the handful that actually appear.
//
// The ones already observed in this game's stream are marked, so the report
// reads as "here is what Web of Shadows uses" rather than "here is PM4".
const Named kOpcodes[] = {
    { 0x10, "ME_INIT?" },
    { 0x21, "COND_WRITE" },              // seen: register read-modify-write
    { 0x22, "DRAW_INDX" },               // draw, with an index buffer
    { 0x23, "VIZ_QUERY" },
    { 0x24, "SET_STATE" },
    { 0x25, "WAIT_FOR_IDLE" },
    { 0x26, "IM_LOAD" },                 // shader upload
    { 0x27, "IM_LOAD_IMMEDIATE" },       // shader inline in the stream
    { 0x2A, "INVALIDATE_STATE" },
    { 0x2B, "SET_CONSTANT" },            // seen: bulk constant/register upload
    { 0x2D, "SET_SHADER_CONSTANTS" },
    { 0x33, "DRAW_INDX_2" },             // draw, indices inline
    { 0x36, "SET_BIN_MASK" },            // seen
    { 0x37, "SET_BIN_SELECT" },
    { 0x3B, "SET_CONSTANT_2" },          // seen
    { 0x3C, "WAIT_REG_MEM" },            // seen: poll a register or memory word
    { 0x3D, "MEM_WRITE" },
    { 0x3F, "INDIRECT_BUFFER" },         // seen: jump into another buffer
    { 0x40, "REG_RMW" },
    { 0x48, "ME_INIT" },                 // seen: first packet of the run
    { 0x50, "INTERRUPT" },
    { 0x55, "EVENT_WRITE" },
    { 0x58, "EVENT_WRITE_SHD" },         // seen: the GPU fence rides on this
    { 0x60, "SET_BIN_MASK_LO" },         // seen
    { 0x61, "SET_BIN_MASK_HI" },         // seen
    { 0x62, "SET_BIN_SELECT_LO" },       // seen
    { 0x63, "SET_BIN_SELECT_HI" },       // seen
};

// Registers worth naming. The Xenos register file is large; these are the ones
// a translator has to read to build a draw call, plus the couple the game
// already polls.
const Named kRegisters[] = {
    { 0x0A31, "CP_polled_by_the_game" },  // seen: the op-0x3C wait target
    { 0x2000, "RB_SURFACE_INFO" },
    { 0x2001, "RB_COLOR_INFO" },
    { 0x2005, "RB_DEPTH_INFO" },
    { 0x2100, "PA_SC_WINDOW_OFFSET" },
    { 0x2104, "PA_SC_WINDOW_SCISSOR_TL" },
    { 0x2105, "PA_SC_WINDOW_SCISSOR_BR" },
    { 0x2280, "PA_SU_SC_MODE_CNTL" },
    { 0x2300, "PA_CL_VPORT_XSCALE" },
    { 0x2301, "PA_CL_VPORT_XOFFSET" },
    { 0x2302, "PA_CL_VPORT_YSCALE" },
    { 0x2303, "PA_CL_VPORT_YOFFSET" },
    { 0x2180, "SQ_PROGRAM_CNTL" },
    { 0x21F7, "SQ_VS_CONST" },
    { 0x21F8, "SQ_PS_CONST" },
    { 0x2200, "RB_MODECONTROL" },
    { 0x2208, "RB_DEPTHCONTROL" },
    { 0x2209, "RB_BLENDCONTROL" },
    { 0x220C, "RB_COLORCONTROL" },
};

struct OpcodeStat { uint32_t opcode; uint64_t count; };
struct RegisterStat { uint32_t reg; uint32_t lastValue; uint64_t writes; };

std::mutex g_mutex;
std::vector<OpcodeStat> g_opcodes;
std::vector<RegisterStat> g_registers;
uint64_t g_draws = 0;

// Bounded: the register index is 15 bits, and an unbounded vector keyed on a
// value the guest controls is a slow leak. In practice a title touches a few
// hundred.
constexpr size_t kMaxRegisters = 512;

const char* Lookup(const Named* table, size_t n, uint32_t id)
{
    for (size_t i = 0; i < n; ++i)
        if (table[i].id == id)
            return table[i].name;
    return nullptr;
}

} // namespace

const char* Pm4OpcodeName(uint32_t opcode)
{
    return Lookup(kOpcodes, sizeof(kOpcodes) / sizeof(kOpcodes[0]), opcode);
}

const char* Pm4RegisterName(uint32_t reg)
{
    return Lookup(kRegisters, sizeof(kRegisters) / sizeof(kRegisters[0]), reg);
}

bool Pm4IsDraw(uint32_t opcode)
{
    return opcode == 0x22 || opcode == 0x33;
}

void Pm4RecordPacket(uint32_t opcode, uint32_t /*count*/)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (Pm4IsDraw(opcode))
        ++g_draws;

    for (auto& o : g_opcodes)
    {
        if (o.opcode == opcode)
        {
            ++o.count;
            return;
        }
    }
    g_opcodes.push_back({ opcode, 1 });
}

void Pm4RecordRegister(uint32_t reg, uint32_t value)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    for (auto& r : g_registers)
    {
        if (r.reg == reg)
        {
            r.lastValue = value;
            ++r.writes;
            return;
        }
    }
    if (g_registers.size() < kMaxRegisters)
        g_registers.push_back({ reg, value, 1 });
}

void Pm4Report()
{
    std::vector<OpcodeStat> ops;
    std::vector<RegisterStat> regs;
    uint64_t draws;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ops = g_opcodes;
        regs = g_registers;
        draws = g_draws;
    }

    if (ops.empty())
        return;

    std::sort(ops.begin(), ops.end(),
        [](const OpcodeStat& a, const OpcodeStat& b) { return a.count > b.count; });

    printf("[pm4] %zu distinct type-3 opcode(s), %llu draw(s):\n",
        ops.size(), (unsigned long long)draws);
    for (const auto& o : ops)
    {
        const char* name = Pm4OpcodeName(o.opcode);
        printf("[pm4]   op 0x%02X %-22s %llu%s\n",
            o.opcode, name ? name : "(unknown)",
            (unsigned long long)o.count,
            Pm4IsDraw(o.opcode) ? "   <- DRAW" : "");
    }

    // Registers, most-written first, capped: the point is to see which ones the
    // game leans on, not to dump the whole file.
    std::sort(regs.begin(), regs.end(),
        [](const RegisterStat& a, const RegisterStat& b) { return a.writes > b.writes; });

    printf("[pm4] %zu register(s) written; busiest:\n", regs.size());
    for (size_t i = 0; i < regs.size() && i < 12; ++i)
    {
        const char* name = Pm4RegisterName(regs[i].reg);
        printf("[pm4]   reg 0x%04X %-26s = 0x%08X (%llu write(s))\n",
            regs[i].reg, name ? name : "",
            regs[i].lastValue, (unsigned long long)regs[i].writes);
    }

    if (draws == 0)
        printf("[pm4]   no draw packets yet — the stream is still device setup, "
               "so there is nothing for a translator to render even if one existed.\n");
}

} // namespace wos::gpu
