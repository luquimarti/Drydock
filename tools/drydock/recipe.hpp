#pragma once

// A recipe describes one game ("ship") that Drydock knows how to recompile.
// It contains no game data: only the product number, which files on the disc
// hold code, where the game loads them, their SHA-256 hashes (so only the
// exact supported version of the disc is accepted) and a list of control
// transfer addresses recorded while playing (trace).
//
// Format: a small TOML subset.
//
//   ship = "TheBortas"
//   title = "PDZ II"
//   region = "USA"
//   product = "MK-81022"
//   trace = "trace.txt"        # relative to the recipe
//
//   [[module]]
//   file = "1ST_READ.PRG"
//   base = 0x06008000
//   sha256 = "..."
//   trace = true               # apply the recipe trace to this module

#include <cstdint>
#include <string>
#include <vector>

namespace drydock {

struct RecipeModule {
    std::string file;
    uint32_t base = 0;
    std::string sha256;
    bool useTrace = false;
};

struct Recipe {
    std::string ship;
    std::string title;
    std::string region;
    std::string product;
    std::string tracePath; // absolute or relative to the working directory once loaded
    std::vector<RecipeModule> modules;

    // "PDZ II (USA)"
    std::string DisplayName() const { return region.empty() ? title : title + " (" + region + ")"; }
};

/// Parses a recipe. On failure returns false with a message in `error`.
bool LoadRecipe(const std::string &path, Recipe &recipe, std::string &error);

} // namespace drydock
