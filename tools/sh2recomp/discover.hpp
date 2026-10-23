#pragma once

// Static function discovery for one SH-2 module image.
//
// Recursive descent over direct control flow from seed entries, plus two
// heuristics for code that direct flow does not reach:
//
//   - bsr targets, and jsr/jmp @Rn where Rn holds a constant loaded from a
//     literal pool (mov.l @(d,PC),Rn ; ... jsr @Rn), start new functions. A
//     resolved jmp is a tail call. Literal-pool slots are marked as data.
//   - By default the SH C calling convention is assumed: r8-r14 survive calls
//     and branch targets inherit them, so a target loaded once in a loop
//     preheader is still known at every call in the loop.
//   - Calls through the BIOS service table (0x06000200-0x060003FF) leave the
//     module and are not counted as unresolved.
//   - Pool pointers: literal-pool constants that point at unclaimed bytes that
//     decode as code become entries (function-pointer tables).
//   - Prologues: unclaimed standard prologues (sts.l pr,@-r15 / mov.l
//     r8..r14,@-r15 / add #-n,r15) right after code, a pool, padding or an
//     rts pair become entries.
//
// A wrong entry is harmless for correctness: SH-2 instructions are 16-bit and
// aligned, so code compiled from any even address is the same code the
// interpreter would run from there, and the runtime only enters it when the
// guest actually jumps there with the same bytes in memory. A wrong entry only
// costs compile time and binary size.

#include <cstdint>
#include <vector>

namespace sh2recomp {

struct DiscoverOptions {
    bool assumeABI = true;     // r8-r14 survive calls, branch targets inherit them
    bool poolPointers = true;  // follow code-looking literal-pool constants
    bool prologues = true;     // scan unclaimed standard prologues
};

struct DiscoverResult {
    std::vector<uint32_t> entries; // sorted function entries (seeds included)
    uint32_t fromFlow = 0, fromPool = 0, fromPrologue = 0;
    uint32_t codeBytes = 0, dataBytes = 0; // classified bytes
    uint32_t unresolvedCalls = 0, unresolvedJumps = 0, biosCalls = 0;
};

DiscoverResult Discover(const std::vector<uint8_t> &img, uint32_t base, const std::vector<uint32_t> &seeds,
                        const DiscoverOptions &opt = {});

} // namespace sh2recomp
