#pragma once
// printf-family formatting for guest calls — see format.cpp for the ABI notes
// and for why the register-sourced path is deliberately bounded.

#include <cstdint>
#include <string>

namespace wos
{

// Where a conversion's argument comes from. Returning false means "no more
// arguments are readable", which the formatter reports inline rather than
// papering over with a zero.
struct ArgSource
{
    virtual ~ArgSource() = default;
    virtual bool next(uint64_t& slot) = 0;
};

// A guest va_list: a pointer to the next 8-byte argument slot.
struct GuestVaListArgs final : ArgSource
{
    uint8_t* base;
    uint32_t cursor;

    GuestVaListArgs(uint8_t* b, uint32_t vaList) : base(b), cursor(vaList) {}
    bool next(uint64_t& slot) override;
};

// Arguments already in registers, in ABI order.
struct RegisterArgs final : ArgSource
{
    uint64_t values[8]{};
    unsigned count = 0;
    unsigned index = 0;

    void add(uint64_t v)
    {
        if (count < 8)
            values[count++] = v;
    }
    bool next(uint64_t& slot) override;
};

// Format a guest format string against a source of arguments.
std::string FormatGuest(uint8_t* base, uint32_t formatAddr, ArgSource& args);

} // namespace wos
