#pragma once

// Shared definitions for the SH-2 differential fuzzer.

#include <cstdint>
#include <cstdio>
#include <vector>

namespace fuzz {

inline constexpr uint32_t kCodeBase = 0x06004000u;   // program + PC-relative pool
inline constexpr uint32_t kWindowBase = 0x06010000u; // data window
inline constexpr uint32_t kRegionSize = 0x1000u;
inline constexpr uint32_t kMaxLen = 64;

struct Program {
    uint32_t R[16];
    uint32_t SR, GBR, VBR, PR;
    uint64_t MAC;
    uint32_t ncode;
    uint16_t code[kMaxLen];
    uint8_t codeRegion[kRegionSize]; // random bytes; code is written over the start
    uint8_t window[kRegionSize];
};

inline bool WritePrograms(const char *path, const std::vector<Program> &p) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint32_t n = (uint32_t)p.size();
    fwrite(&n, 4, 1, f);
    fwrite(p.data(), sizeof(Program), p.size(), f);
    fclose(f);
    return true;
}

inline bool ReadPrograms(const char *path, std::vector<Program> &p) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint32_t n = 0;
    if (fread(&n, 4, 1, f) != 1) { fclose(f); return false; }
    p.resize(n);
    const bool ok = fread(p.data(), sizeof(Program), n, f) == n;
    fclose(f);
    return ok;
}

} // namespace fuzz
