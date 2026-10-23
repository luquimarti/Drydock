#pragma once

// Recompiles the SH-2 code of a game to C++ (in memory).
//
// Each module (the main executable, and every overlay file the game loads
// later, possibly at the same address as other overlays) becomes
// <NAME>.cpp (registration tables) plus <NAME>_NN.cpp (code, a few MB each);
// game.cpp exports drydock_abi_version / drydock_register (abi.h). Compile
// every file into one library, or link them into the emulator.
//
// Function entries come from:
//   - static discovery (direct flow, literal-pool call targets, pool pointers,
//     standard prologues; see discover.hpp), seeded with the module start,
//     the given entries and trace targets
//   - control-transfer traces recorded by `ymir-headless --trace` (call, jump
//     and exception targets start functions; branch and return targets that
//     no function covers start extra functions)
//
// Every label is registered with the runtime together with the code word it
// was compiled from and its module; the runtime only enters native code when
// those bytes, and a set of check words spread over the module, are in memory,
// so overlays sharing an address never run each other's code.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sh2recomp {

struct TraceTarget {
    uint32_t address;
    uint32_t kinds; // 1 = call, 2 = jump, 4 = branch, 8 = return, 16 = exception
};

/// Reads a trace written by `ymir-headless --trace` ("ADDRESS KINDS" lines).
bool LoadTrace(const std::string &path, std::vector<TraceTarget> &out);

struct ModuleInput {
    std::string name; // e.g. "EPISODE1.PRG"
    uint32_t base = 0;
    std::vector<uint8_t> image;
    std::vector<TraceTarget> trace;
    std::vector<uint32_t> entries;
};

struct GeneratedFile {
    std::string name; // file name, no directory
    std::string contents;
};

struct GameOutput {
    std::vector<GeneratedFile> files;
    std::vector<std::string> reports; // one line per module
    size_t entryPoints = 0;
};

/// Progress: modules done so far, total modules, name of the module just done.
using GenerateProgress = std::function<void(size_t done, size_t total, const std::string &module)>;

GameOutput GenerateGame(const std::vector<ModuleInput> &modules, const std::string &product,
                        bool staticDiscovery = true, const GenerateProgress &progress = {});

} // namespace sh2recomp
