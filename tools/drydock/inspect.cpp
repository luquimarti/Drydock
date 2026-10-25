#include "inspect.hpp"

#include "build.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <cctype>
#include <iterator>
#include <map>
#include <tuple>

namespace fs = std::filesystem;

namespace drydock {

namespace {

    // Work RAM regions as written by `ymir-headless --dump FRAME:PREFIX`.
    struct Region {
        std::string dump;  // prefix
        std::string name;  // "HWRAM" / "LWRAM"
        uint32_t base = 0; // SH-2 address of the first byte
        std::vector<uint8_t> bytes;
    };

    struct Location {
        bool found = false;
        uint32_t address = 0;
        double match = 0.0; // fraction of the file's bytes found identical at that address
        std::string dump;
    };

    bool LoadBytes(const fs::path &path, std::vector<uint8_t> &out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return !out.empty();
    }

    fs::path P(const std::string &s) {
        return fs::path(std::u8string(s.begin(), s.end()));
    }

    // Best place where `data` appears in one of the dumps: the occurrence of its first
    // bytes with the most identical bytes over the whole file (code may be patched or
    // followed by uninitialized data after loading, so it need not be a perfect match).
    Location Locate(const std::vector<uint8_t> &data, const std::vector<Region> &regions) {
        Location best;
        constexpr size_t kProbe = 64;
        if (data.size() < kProbe) return best;
        // A probe made of one repeated byte (padding, zeros) would match anywhere
        if (std::all_of(data.begin(), data.begin() + kProbe, [&](uint8_t b) { return b == data[0]; })) return best;

        const std::boyer_moore_horspool_searcher searcher(data.begin(), data.begin() + kProbe);
        for (const Region &r : regions) {
            auto it = r.bytes.begin();
            for (int hits = 0; hits < 64; hits++) {
                it = std::search(it, r.bytes.end(), searcher);
                if (it == r.bytes.end()) break;
                const size_t off = (size_t)(it - r.bytes.begin());
                const size_t span = std::min(data.size(), r.bytes.size() - off);
                size_t same = 0;
                for (size_t i = 0; i < span; i++) same += data[i] == r.bytes[off + i];
                const double match = (double)same / (double)data.size();
                if (match > best.match) {
                    best.found = true;
                    best.address = r.base + (uint32_t)off;
                    best.match = match;
                    best.dump = r.dump + " " + r.name;
                }
                ++it;
            }
        }
        return best;
    }

    std::string RegionName(const std::string &areas) {
        static const std::map<char, std::string> names = {
            {'U', "USA"}, {'J', "Japan"}, {'E', "Europe"}, {'T', "Asia NTSC"}, {'A', "Asia PAL"},
            {'B', "Central/South America NTSC"}, {'K', "Korea"}, {'L', "Central/South America PAL"}};
        std::string out;
        for (char c : areas) {
            auto it = names.find(c);
            if (it == names.end()) continue;
            if (!out.empty()) out += ", ";
            out += it->second;
        }
        return out;
    }

    std::string Upper(std::string s) {
        for (char &c : s) c = (char)toupper((unsigned char)c);
        return s;
    }

    // Extensions that usually hold SH-2 code on Saturn discs (or no extension at all).
    bool LooksLikeCode(const std::string &path) {
        const std::string name = Upper(path.substr(path.find_last_of('/') + 1));
        const size_t dot = name.find_last_of('.');
        if (dot == std::string::npos) return true;
        const std::string ext = name.substr(dot + 1);
        for (const char *e : {"PRG", "BIN", "EXE", "OVL", "SH2", "OBJ", "ABS", "COF", "MOD", "DLL"})
            if (ext == e) return true;
        return false;
    }

    std::string Hex(uint32_t v) {
        char buf[16];
        snprintf(buf, sizeof buf, "0x%08X", v);
        return buf;
    }

} // namespace

int RunInspect(const std::string &discPath, const std::vector<std::string> &dumpPrefixes) {
    std::vector<Region> regions;
    for (const std::string &prefix : dumpPrefixes) {
        bool any = false;
        for (const auto &[suffix, name, base] : {std::tuple{"-hwram.bin", "HWRAM", 0x06000000u},
                                                 std::tuple{"-lwram.bin", "LWRAM", 0x00200000u}}) {
            Region r{prefix, name, base, {}};
            if (LoadBytes(P(prefix + suffix), r.bytes)) {
                regions.push_back(std::move(r));
                any = true;
            }
        }
        if (!any) {
            fprintf(stderr, "No memory dump found for %s (expected %s-hwram.bin / %s-lwram.bin)\n", prefix.c_str(),
                    prefix.c_str(), prefix.c_str());
            return 1;
        }
    }

    std::map<std::string, Location> where;
    const DiscInfo info = InspectDisc(discPath, [&](const DiscFile &f, const std::vector<uint8_t> &data) {
        if (!regions.empty()) where[f.path] = Locate(data, regions);
    });
    if (!info.ok) {
        fprintf(stderr, "%s\n", info.error.c_str());
        return 1;
    }

    const std::string region = RegionName(info.areaCodes);
    printf("Title      %s\n", info.title.c_str());
    printf("Product    %s\n", info.product.c_str());
    printf("Version    %s   Date %s\n", info.version.c_str(), info.releaseDate.c_str());
    printf("Areas      %s (%s)\n", info.areaCodes.c_str(), region.c_str());
    printf("Boot file  %s, loaded at %s\n", info.bootFile.c_str(), Hex(info.firstReadAddress).c_str());
    printf("\n%-10s %-64s  %s\n", "Size", "SHA-256", "File");
    for (const DiscFile &f : info.files) {
        printf("%-10u %-64s  %s", f.size, f.readable ? f.sha256.c_str() : "(unreadable)", f.path.c_str());
        auto it = where.find(f.path);
        if (it != where.end() && it->second.found)
            printf("   -> %s (%.1f%% in %s)", Hex(it->second.address).c_str(), it->second.match * 100.0,
                   it->second.dump.c_str());
        printf("\n");
    }

    // Recipe skeleton
    printf("\n# ---- recipe skeleton: ship/recipe.toml -------------------------------------\n");
    printf("ship = \"NewShip\"\n");
    printf("title = \"%s\"\n", info.title.c_str());
    printf("region = \"%s\"\n", region.c_str());
    printf("product = \"%s\"\n", info.product.c_str());
    printf("trace = \"trace.txt\"\n");
    auto module = [&](const DiscFile &f, uint32_t base, bool trace) {
        printf("\n[[module]]\nfile = \"%s\"\nbase = %s\nsha256 = \"%s\"\n", f.path.c_str(), Hex(base).c_str(),
               f.sha256.c_str());
        if (trace) printf("trace = true\n");
    };
    std::vector<const DiscFile *> unknown;
    for (const DiscFile &f : info.files) {
        if (!f.readable) continue;
        if (f.path == info.bootFile) {
            module(f, info.firstReadAddress, true);
            continue;
        }
        auto it = where.find(f.path);
        const bool code = LooksLikeCode(f.path);
        if (it != where.end() && it->second.found && (code || it->second.match >= 0.99)) {
            if (it->second.match < 0.9)
                printf("\n# %s: only %.1f%% identical at %s, check before using\n", f.path.c_str(),
                       it->second.match * 100.0, Hex(it->second.address).c_str());
            module(f, it->second.address, false);
        } else if (code) {
            unknown.push_back(&f);
        }
    }
    if (!unknown.empty()) {
        printf("\n# Possible code files with no known load address%s:\n",
               regions.empty() ? " (pass --dump PREFIX to find them)" : " (not found in the dumps)");
        for (const DiscFile *f : unknown) printf("#   %s (%u bytes)\n", f->path.c_str(), f->size);
    }
    if (regions.empty())
        printf("\n# Tip: dump memory while each part of the title is loaded, e.g.\n"
               "#   ymir-headless bios.bin disc.cue 3000 --press 900:906:start --dump 3000:race\n"
               "# then run again with --dump race to find where each file is loaded.\n");
    return 0;
}

} // namespace drydock
