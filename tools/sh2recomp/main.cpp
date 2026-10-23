// sh2recomp: recompiles the SH-2 code of a game to C++ (see generate.hpp).
//
//   sh2recomp --out DIR [--product MK-81022] [--no-static]
//             --module NAME FILE LOAD-ADDRESS [--trace trace.txt]... [--entry ADDR]...
//             [--module ...]

#include "generate.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

bool ReadFile(const std::string &path, std::vector<uint8_t> &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return true;
}

int Usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s --out DIR [--product ID] [--no-static]\n"
            "          --module NAME FILE LOAD-ADDRESS [--trace FILE]... [--entry ADDR]... [--module ...]\n",
            argv0);
    return 2;
}

} // namespace

int main(int argc, char **argv) {
    std::string outDir, product;
    bool staticDiscovery = true;
    std::vector<sh2recomp::ModuleInput> modules;

    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) outDir = argv[++i];
        else if (a == "--product" && i + 1 < argc) product = argv[++i];
        else if (a == "--no-static") staticDiscovery = false;
        else if (a == "--module" && i + 3 < argc) {
            sh2recomp::ModuleInput m;
            m.name = argv[++i];
            const std::string file = argv[++i];
            m.base = (uint32_t)strtoul(argv[++i], nullptr, 0);
            if (!ReadFile(file, m.image)) {
                fprintf(stderr, "cannot read %s\n", file.c_str());
                return 1;
            }
            modules.push_back(std::move(m));
        } else if (a == "--trace" && i + 1 < argc && !modules.empty()) {
            if (!sh2recomp::LoadTrace(argv[++i], modules.back().trace)) {
                fprintf(stderr, "cannot read %s\n", argv[i]);
                return 1;
            }
        } else if (a == "--entry" && i + 1 < argc && !modules.empty()) {
            modules.back().entries.push_back((uint32_t)strtoul(argv[++i], nullptr, 0));
        } else {
            fprintf(stderr, "unexpected argument: %s\n", a.c_str());
            return Usage(argv[0]);
        }
    }
    if (modules.empty() || outDir.empty()) return Usage(argv[0]);

    const sh2recomp::GameOutput out = sh2recomp::GenerateGame(modules, product, staticDiscovery);
    for (const std::string &r : out.reports) printf("%s\n", r.c_str());
    for (const sh2recomp::GeneratedFile &f : out.files) {
        const std::string path = outDir + "/" + f.name;
        FILE *o = fopen(path.c_str(), "wb");
        if (!o) {
            fprintf(stderr, "cannot write %s\n", path.c_str());
            return 1;
        }
        fwrite(f.contents.data(), 1, f.contents.size(), o);
        fclose(o);
    }
    printf("total: %zu modules, %zu registered entry points, %zu files\n", modules.size(), out.entryPoints,
           out.files.size());
    return 0;
}
