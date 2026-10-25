#pragma once

// Drydock build: turns the player's own disc into a native game library.
//
//   1. CheckDisc   reads the code files named by the recipe from the disc
//                  image and accepts it only if the product number and every
//                  file hash match (fast; used to refuse a wrong disc)
//   2. BuildGame   recompiles them (sh2recomp), compiles the C++ with a bare
//                  Clang (no system headers, no C runtime) and links
//                  <dir>/recomp/<product>.dll (.so elsewhere)
//
// Nothing derived from the game leaves the player's machine.

#include "recipe.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace drydock {

struct DiscCheck {
    bool ok = false;
    std::string error;                       // user-facing, English
    std::vector<std::vector<uint8_t>> files; // one per recipe module, when ok
};

/// Reads and verifies the disc image (.cue/.chd/.iso/...) against the recipe.
DiscCheck CheckDisc(const Recipe &recipe, const std::string &discPath);

/// What a disc image contains: header fields and every file, for writing a new recipe.
struct DiscFile {
    std::string path; // "FILE.BIN" or "DIR/FILE.BIN"
    uint32_t lba = 0;
    uint32_t size = 0;
    bool readable = false;
    std::string sha256;
};

struct DiscInfo {
    bool ok = false;
    std::string error;
    std::string title, product, version, releaseDate, areaCodes;
    uint32_t firstReadAddress = 0; // load address of the boot file (IP.BIN)
    uint32_t firstReadSize = 0;
    std::string bootFile;          // first file of the root directory
    std::vector<DiscFile> files;   // directory order
};

/// Called once per readable file with its contents (optional).
using DiscFileVisitor = std::function<void(const DiscFile &file, const std::vector<uint8_t> &data)>;

/// Reads the disc header and lists every file with its size and hash.
DiscInfo InspectDisc(const std::string &discPath, const DiscFileVisitor &visit = {});

struct Toolchain {
    std::string clang;  // clang / clang.exe
    std::string linker; // lld-link.exe on Windows; empty: clang -shared
    unsigned jobs = 0;  // parallel compiles (0: hardware threads)
    bool keepWork = false;
};

/// Toolchain shipped next to the executable: <exeDir>/tools/llvm/...
Toolchain BundledToolchain(const std::string &exeDir);

enum class Stage { CheckingDisc, Generating, Compiling, Linking, Done };
const char *StageName(Stage stage);

/// fraction: 0..1 within the current stage; detail: what is being worked on.
using Progress = std::function<void(Stage stage, float fraction, const std::string &detail)>;

struct BuildResult {
    bool ok = false;
    std::string error;
    std::string libraryPath;
};

/// Builds <dir>/recomp/<product>.<dll|so>. `cancel` may be null.
BuildResult BuildGame(const Recipe &recipe, const std::string &discPath, const std::string &dir,
                      const Toolchain &toolchain, const Progress &progress = {},
                      const std::atomic<bool> *cancel = nullptr);

/// True if <dir>/recomp has a library built from this recipe for this ABI.
bool IsLibraryCurrent(const Recipe &recipe, const std::string &dir);

/// Path of the game library for `dir` and the recipe's product.
std::string LibraryPath(const Recipe &recipe, const std::string &dir);

} // namespace drydock
