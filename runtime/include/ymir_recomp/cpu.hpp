#pragma once

// Direct access to Ymir's SH-2 state for recompiled code.
//
// Requires patches/0001-sh2-recomp-interface.patch, which declares
// ymir_recomp::CPU a friend of ymir::sh2::SH2 and adds the RecompRead*/
// RecompWrite* memory accessors. Everything here is header-only so the
// compiler can inline register accesses into generated code.

#include <ymir/hw/sh2/sh2.hpp>

#include "abi.h"

#include <array>
#include <cstdint>

namespace ymir_recomp {

struct CPU {
    using SH2 = ymir::sh2::SH2;

    static std::array<uint32_t, 16> &R(SH2 &c) { return c.R; }
    static uint32_t &PC(SH2 &c) { return c.PC; }
    static uint32_t &PR(SH2 &c) { return c.PR; }
    static uint32_t &GBR(SH2 &c) { return c.GBR; }
    static uint32_t &VBR(SH2 &c) { return c.VBR; }
    static ymir::sh2::RegSR &SR(SH2 &c) { return c.SR; }
    static ymir::sh2::RegMAC &MAC(SH2 &c) { return c.MAC; }

    // Cycles executed so far in the current SH2::Advance() call.
    static uint64_t &Cycles(SH2 &c) { return c.m_cyclesExecuted; }

    // Register written by the previous instruction's load (or kWBNone/kWBPR):
    // an instruction that reads it right after stalls one cycle. Recompiled
    // code charges the same stall as the interpreter and keeps this up to date.
    static uint8_t &WB(SH2 &c) { return c.m_wbReg; }
    static constexpr uint8_t kWBNone = 0xFF;
    static constexpr uint8_t kWBPR = 0x10;

    // After LDC/STC/LDS/STS the SH-2 does not accept an interrupt before the
    // next instruction. Recompiled blocks that end right after one of them
    // must preserve that.
    static void InhibitInterruptsOneInstruction(SH2 &c) { c.m_intrFlags.allow = false; }

    // True while the interpreter is between a delayed branch and its slot.
    // Native code must never be entered then.
    static bool InDelaySlot(SH2 &c) { return c.m_delaySlot; }

    // Builds the Drydock ABI view of this SH-2 (abi.h) that recompiled code
    // runs against: pointers into the SH-2 state plus memory accessors.
    static drydock::Cpu MakeContext(SH2 &c);
};

} // namespace ymir_recomp
