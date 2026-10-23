#pragma once

// SH-2 -> C++ emitter.
//
// Each guest instruction becomes a few lines of C++ that operate on Ymir's SH-2
// state through the Drydock ABI (runtime/include/ymir_recomp/abi.h). The
// semantics mirror Ymir's interpreter (libs/ymir-core/src/ymir/hw/sh2/sh2.cpp)
// instruction by instruction; tests/fuzz checks that claim against the
// interpreter itself.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sh2gen {

/// Result of emitting one instruction.
enum class EmitStatus {
    Ok,          ///< emitted; execution continues with the next instruction
    Unsupported, ///< not handled natively: the block must end before it and
                 ///< let the interpreter execute it
};

struct Emitted {
    EmitStatus status = EmitStatus::Ok;
    std::string mnemonic;       ///< for comments in the generated code
    std::string code;           ///< C++ statements (empty for nop)
    uint32_t cycles = 1;        ///< fixed cost; memory access cycles are charged by the accessors in `code`
    bool inhibitsInterrupt = false; ///< LDC/STC/LDS/STS: no interrupt right after

    // Load-use stall model, mirroring Ymir's interpreter: one extra cycle if
    // any register in `wbRead` was loaded by the previous instruction (counted
    // twice if `wbTwice`), then the instruction leaves `wbSet` as the pending
    // load (0-15, 0x10 = PR, 0xFF = none).
    std::vector<uint8_t> wbRead;
    bool wbTwice = false;
    uint8_t wbSet = 0xFF;
};

/// Emits a single non-branch instruction located at `pc`.
///
/// `slotPC`, when given, means the instruction executes in the delay slot of a
/// taken branch: it is a C++ expression for the PC value Ymir uses for
/// PC-relative operands there (branch target - 2).
Emitted EmitInstruction(uint16_t opcode, uint32_t pc, const char *slotPC = nullptr);

/// Reads one instruction word; returns false outside known code.
using CodeReader = std::function<bool(uint32_t addr, uint16_t &op)>;

struct FunctionInfo {
    std::string name;
    uint32_t entry = 0;
    std::vector<uint32_t> labels;      ///< addresses the function can be entered at
    std::vector<uint32_t> callTargets; ///< direct bsr targets (candidates for more functions)
    uint32_t instructions = 0;         ///< instructions compiled (delay slots included)
    uint32_t interpreterExits = 0;     ///< places that hand an instruction to the interpreter
};

/// Emits the function reachable from `entry` through direct control flow:
///
///     void <name>(drydock::Cpu &cpu)
///
/// The function is entered by `switch (PC)` over every label, so the
/// dispatcher can resume it at any block start: after a call returns, or after
/// it yielded at a backward branch because the cycle budget ran out or an
/// interrupt became pending. Calls and computed jumps leave through the
/// dispatcher (PC set to the target); instructions the emitter does not handle
/// leave with PC pointing at them for the interpreter.
///
/// `extraEntries` are further addresses known to be reached (from pool-constant
/// analysis or from runtime call logs, e.g. jump-table targets); they become
/// labels and are compiled even if direct flow does not reach them.
FunctionInfo EmitFunction(const std::string &name, uint32_t entry, const CodeReader &read, std::string &out,
                          const std::vector<uint32_t> &extraEntries = {});

/// Prologue shared by every generated translation unit.
std::string FilePrologue();

} // namespace sh2gen
