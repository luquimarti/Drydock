#include "generate.hpp"

#include "../sh2gen/emit.hpp"
#include "discover.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>

namespace sh2recomp {

namespace {

    // C identifier from a module name ("EPISODE1.PRG" -> "EPISODE1_PRG").
    std::string Ident(const std::string &name) {
        std::string s;
        for (char ch : name) s += (isalnum((unsigned char)ch) ? ch : '_');
        if (s.empty() || isdigit((unsigned char)s[0])) s = "m_" + s;
        return s;
    }

    struct ModuleResult {
        std::string ident;
        size_t labels = 0;
    };

    void GenerateModule(const ModuleInput &in, bool staticDiscovery, GameOutput &out, ModuleResult &result) {
        const std::vector<uint8_t> &img = in.image;
        const uint32_t base = in.base;
        const uint32_t end = base + (uint32_t)(img.size() & ~size_t(1));
        const std::string id = Ident(in.name);
        result.ident = id;

        std::set<uint32_t> starts(in.entries.begin(), in.entries.end());
        std::set<uint32_t> others; // branch / return targets
        unsigned outside = 0;
        for (const TraceTarget &t : in.trace) {
            if (t.address < base || t.address >= end) {
                outside++;
                continue;
            }
            if (t.kinds & (1 | 2 | 16)) starts.insert(t.address);
            else others.insert(t.address);
        }

        DiscoverResult disc;
        if (staticDiscovery) {
            std::vector<uint32_t> seeds(starts.begin(), starts.end());
            seeds.push_back(base);
            disc = Discover(img, base, seeds);
            starts.insert(disc.entries.begin(), disc.entries.end());
        }

        auto read = [&](uint32_t addr, uint16_t &op) {
            if (addr < base || addr + 1 >= end || (addr & 1)) return false;
            op = (uint16_t)((img[addr - base] << 8) | img[addr - base + 1]);
            return true;
        };
        auto word = [&](uint32_t pc) {
            const uint32_t a = (pc & ~3u) - base;
            if ((pc & ~3u) < base || a + 3 >= img.size()) return 0u;
            return ((uint32_t)img[a] << 24) | ((uint32_t)img[a + 1] << 16) | ((uint32_t)img[a + 2] << 8) | img[a + 3];
        };

        // Functions go to chunk files of a few MB (<NAME>_NN.cpp) so they
        // compile in parallel and no file gets huge; the registration tables
        // go to <NAME>.cpp.
        const size_t kChunkBytes = 6u << 20;
        const std::string chunkHeader =
            sh2gen::FilePrologue() + "// module " + in.name + "\nnamespace ymir_recomp_gen {\n\n";
        std::vector<std::string> chunks;
        std::string cpp = chunkHeader;
        std::string decls;

        // label -> function that owns it (a function's own entry wins over
        // overlapping code compiled as part of another function)
        std::map<uint32_t, std::string> owner;
        uint64_t instrs = 0, exits = 0;
        unsigned functions = 0;
        std::set<uint32_t> emitted;

        auto emit = [&](uint32_t entry) {
            if (!emitted.insert(entry).second) return;
            char name[96];
            snprintf(name, sizeof name, "%s_%08X", id.c_str(), entry);
            const sh2gen::FunctionInfo fi = sh2gen::EmitFunction(name, entry, read, cpp);
            cpp += "\n";
            decls += std::string("void ") + name + "(drydock::Cpu &cpu);\n";
            functions++;
            instrs += fi.instructions;
            exits += fi.interpreterExits;
            for (uint32_t l : fi.labels) {
                if (l == entry) owner[l] = name;
                else if (!owner.count(l)) owner[l] = name;
            }
            if (cpp.size() >= kChunkBytes) {
                chunks.push_back(cpp + "} // namespace ymir_recomp_gen\n");
                cpp = chunkHeader;
            }
        };

        for (uint32_t e : starts)
            if (e >= base && e < end) emit(e);
        unsigned extra = 0;
        for (uint32_t t : others)
            if (!owner.count(t)) {
                emit(t);
                extra++;
            }
        if (cpp.size() > chunkHeader.size()) chunks.push_back(cpp + "} // namespace ymir_recomp_gen\n");

        // Check words: code words at function entries spread evenly over the
        // module. Code never changes while the module is loaded, so they
        // identify which overlay occupies the address range right now.
        std::vector<uint32_t> entryList;
        for (const auto &[pc, fn] : owner)
            if (word(pc) != 0) entryList.push_back(pc & ~3u);
        std::sort(entryList.begin(), entryList.end());
        entryList.erase(std::unique(entryList.begin(), entryList.end()), entryList.end());
        std::vector<uint32_t> checks;
        const size_t kChecks = 16;
        for (size_t i = 0; i < kChecks && !entryList.empty(); i++) {
            const uint32_t a = entryList[i * entryList.size() / kChecks];
            if (checks.empty() || checks.back() != a) checks.push_back(a);
        }

        std::string reg = "// Generated by sh2recomp. Do not edit.\n// module " + in.name +
                          "\n#include <ymir_recomp/abi.h>\n\nnamespace ymir_recomp_gen {\n" + decls +
                          "} // namespace ymir_recomp_gen\n\nusing namespace ymir_recomp_gen;\n\n";
        char line[256];
        reg += "static const drydock::Check kChecks_" + id + "[] = {\n";
        for (uint32_t a : checks) {
            snprintf(line, sizeof line, "    {0x%08Xu, 0x%08Xu},\n", a, word(a));
            reg += line;
        }
        reg += "};\n\nstatic const drydock::Entry kEntries_" + id + "[] = {\n";
        for (const auto &[pc, fn] : owner) {
            snprintf(line, sizeof line, "    {0x%08Xu, 0x%08Xu, %s},\n", pc, word(pc), fn.c_str());
            reg += line;
        }
        reg += "};\n\n";
        reg += "void RegisterModule_" + id + "(drydock::Registrar &rt) {\n";
        snprintf(line, sizeof line, "    const uint16_t m = rt.addModule(rt.ctx, \"%s\", 0x%08Xu, 0x%Xu, kChecks_%s,\n",
                 in.name.c_str(), base, (unsigned)img.size(), id.c_str());
        reg += line;
        reg += "                                    sizeof(kChecks_" + id + ") / sizeof(kChecks_" + id + "[0]));\n";
        reg += "    rt.addEntries(rt.ctx, m, kEntries_" + id + ", sizeof(kEntries_" + id + ") / sizeof(kEntries_" +
               id + "[0]));\n";
        reg += "}\n";

        size_t bytes = reg.size();
        out.files.push_back({id + ".cpp", std::move(reg)});
        for (size_t k = 0; k < chunks.size(); k++) {
            char suffix[16];
            snprintf(suffix, sizeof suffix, "_%02zu.cpp", k);
            bytes += chunks[k].size();
            out.files.push_back({id + suffix, std::move(chunks[k])});
        }
        result.labels = owner.size();

        std::string report;
        snprintf(line, sizeof line, "%-14s 0x%08X-0x%08X %7zu bytes | %6u functions", in.name.c_str(), base, end,
                 img.size(), functions);
        report += line;
        if (staticDiscovery) {
            snprintf(line, sizeof line, " (static: %zu = %u flow, %u pool ptr, %u prologue; code %u KB, data %u KB)",
                     disc.entries.size(), disc.fromFlow, disc.fromPool, disc.fromPrologue, disc.codeBytes / 1024,
                     disc.dataBytes / 1024);
            report += line;
        }
        snprintf(line, sizeof line,
                 " | %zu labels, %llu instr, %llu interp exits, %u extra, %u trace targets elsewhere | %.1f MB in %zu files",
                 owner.size(), (unsigned long long)instrs, (unsigned long long)exits, extra, outside,
                 bytes / 1048576.0, chunks.size() + 1);
        report += line;
        out.reports.push_back(report);
    }

} // namespace

bool LoadTrace(const std::string &path, std::vector<TraceTarget> &out) {
    FILE *t = fopen(path.c_str(), "r");
    if (!t) return false;
    char line[128];
    while (fgets(line, sizeof line, t)) {
        unsigned addr, kinds;
        if (line[0] == '#' || sscanf(line, "%x %u", &addr, &kinds) != 2) continue;
        out.push_back({addr, kinds});
    }
    fclose(t);
    return true;
}

GameOutput GenerateGame(const std::vector<ModuleInput> &modules, const std::string &product, bool staticDiscovery,
                        const GenerateProgress &progress) {
    GameOutput out;
    std::vector<ModuleResult> results;
    for (size_t i = 0; i < modules.size(); i++) {
        ModuleResult r;
        GenerateModule(modules[i], staticDiscovery, out, r);
        results.push_back(r);
        out.entryPoints += r.labels;
        if (progress) progress(i + 1, modules.size(), modules[i].name);
    }

    // the library's exported entry points
    std::string decls, body;
    for (const ModuleResult &r : results) {
        decls += "void RegisterModule_" + r.ident + "(drydock::Registrar &rt);\n";
        body += "    RegisterModule_" + r.ident + "(*rt);\n";
    }
    std::string game = "// Generated by sh2recomp: registers every recompiled module of the game.\n"
                       "#include <ymir_recomp/abi.h>\n\n" +
                       decls +
                       "\nDRYDOCK_EXPORT int drydock_abi_version() {\n    return DRYDOCK_ABI_VERSION;\n}\n\n"
                       "DRYDOCK_EXPORT void drydock_register(drydock::Registrar *rt) {\n";
    if (!product.empty()) game += "    rt->setProduct(rt->ctx, \"" + product + "\");\n";
    game += body + "}\n";
    out.files.push_back({"game.cpp", std::move(game)});
    return out;
}

} // namespace sh2recomp
