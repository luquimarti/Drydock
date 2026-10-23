// sh2fuzz-run <programs.bin> [--verbose]
//
// Runs every generated program twice on the same Ymir machine: once through
// Ymir's own SH-2 interpreter and once through the C++ emitted by sh2gen, from
// identical initial state, and compares the architectural state afterwards
// (R0-R15, PC, SR, GBR, VBR, PR, MAC) plus both 4 KiB memory regions, the
// total number of cycles charged and the pending-load register (load-use
// stall state) at the end.

#include "fuzz_common.hpp"

#include <ymir/ymir.hpp>
#include <ymir_recomp/cpu.hpp>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

#include <ymir_recomp/dispatch.hpp>

namespace fuzz_gen {
using RegFn = void (*)(ymir_recomp::Table &);
extern const RegFn g_register[];
extern const unsigned g_programCount;
} // namespace fuzz_gen

using namespace fuzz;
using ymir_recomp::CPU;

namespace {

struct State {
    uint32_t R[16], PC, SR, GBR, VBR, PR;
    uint64_t MAC;
    uint64_t cycles; // total cycles charged for the program
    uint8_t wb;      // pending load at the end
    uint8_t code[kRegionSize], window[kRegionSize];
};

void Load(ymir::sh2::SH2 &cpu, const Program &p) {
    auto &probe = cpu.GetProbe();
    for (uint32_t i = 0; i < kRegionSize; i++) {
        probe.MemPokeByte(kCodeBase + i, p.codeRegion[i], true);
        probe.MemPokeByte(kWindowBase + i, p.window[i], true);
    }
    for (int i = 0; i < 16; i++) CPU::R(cpu)[i] = p.R[i];
    CPU::SR(cpu).u32 = p.SR;
    CPU::GBR(cpu) = p.GBR;
    CPU::VBR(cpu) = p.VBR;
    CPU::PR(cpu) = p.PR;
    CPU::MAC(cpu).u64 = p.MAC;
    CPU::PC(cpu) = kCodeBase;
    CPU::WB(cpu) = CPU::kWBNone;
    probe.RefillPipeline();
}

State Snapshot(ymir::sh2::SH2 &cpu, uint64_t cycles) {
    State s{};
    s.cycles = cycles;
    s.wb = CPU::WB(cpu);
    auto &probe = cpu.GetProbe();
    for (int i = 0; i < 16; i++) s.R[i] = CPU::R(cpu)[i];
    s.PC = CPU::PC(cpu);
    s.SR = CPU::SR(cpu).u32;
    s.GBR = CPU::GBR(cpu);
    s.VBR = CPU::VBR(cpu);
    s.PR = CPU::PR(cpu);
    s.MAC = CPU::MAC(cpu).u64;
    for (uint32_t i = 0; i < kRegionSize; i++) {
        s.code[i] = probe.MemPeekByte(kCodeBase + i, true);
        s.window[i] = probe.MemPeekByte(kWindowBase + i, true);
    }
    return s;
}

int Compare(const State &a, const State &b, std::string &why) {
    char buf[256];
    int diffs = 0;
    auto add = [&](const char *what, uint64_t x, uint64_t y) {
        if (x == y) return;
        diffs++;
        snprintf(buf, sizeof buf, "    %-6s interp=%016llX native=%016llX\n", what, (unsigned long long)x,
                 (unsigned long long)y);
        why += buf;
    };
    for (int i = 0; i < 16; i++) {
        char n[8];
        snprintf(n, sizeof n, "R%d", i);
        add(n, a.R[i], b.R[i]);
    }
    add("PC", a.PC, b.PC);
    add("SR", a.SR, b.SR);
    add("GBR", a.GBR, b.GBR);
    add("VBR", a.VBR, b.VBR);
    add("PR", a.PR, b.PR);
    add("MAC", a.MAC, b.MAC);
    add("cycles", a.cycles, b.cycles);
    add("WB", a.wb, b.wb);
    for (uint32_t i = 0; i < kRegionSize; i++) {
        if (a.window[i] != b.window[i]) {
            diffs++;
            snprintf(buf, sizeof buf, "    mem %08X interp=%02X native=%02X\n", kWindowBase + i, a.window[i], b.window[i]);
            why += buf;
        }
        if (a.code[i] != b.code[i]) {
            diffs++;
            snprintf(buf, sizeof buf, "    mem %08X interp=%02X native=%02X\n", kCodeBase + i, a.code[i], b.code[i]);
            why += buf;
        }
    }
    return diffs;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s programs.bin [--verbose]\n", argv[0]);
        return 2;
    }
    const bool verbose = argc > 2 && !strcmp(argv[2], "--verbose");
    std::vector<Program> progs;
    if (!ReadPrograms(argv[1], progs) || progs.size() != fuzz_gen::g_programCount) {
        fprintf(stderr, "cannot read %s (or it does not match the compiled programs)\n", argv[1]);
        return 1;
    }

    auto sat = std::make_unique<ymir::Saturn>();
    sat->Reset(true);
    ymir::sh2::SH2 &cpu = sat->masterSH2;
    drydock::Cpu ctx = CPU::MakeContext(cpu); // what recompiled code runs against

    unsigned failed = 0;
    uint64_t nativeCalls = 0, interpSteps = 0, noProgress = 0, notFound = 0, inSlot = 0, yieldRuns = 0;
    for (size_t i = 0; i < progs.size(); i++) {
        const Program &p = progs[i];
        const uint32_t end = kCodeBase + p.ncode * 2;
        static constexpr unsigned kMaxSteps = 20000;

        // Reference: Ymir's interpreter only.
        Load(cpu, p);
        unsigned steps = 0;
        uint64_t cycles = 0;
        // SH2::Step() leaves the instruction's cost in the cycle counter
        auto step = [&] {
            sat->StepMasterSH2();
            cycles += CPU::Cycles(cpu);
        };
        while (CPU::PC(cpu) != end && steps < kMaxSteps) {
            step();
            steps++;
        }
        const State ref = Snapshot(cpu, cycles);

        // Mixed: native code wherever the table has an entry, the interpreter
        // for everything else -- the same policy the SH2::Advance hook uses.
        ymir_recomp::Table table;
        fuzz_gen::g_register[i](table);
        Load(cpu, p);
        steps = 0;
        cycles = 0;
        while (CPU::PC(cpu) != end && steps < kMaxSteps) {
            const uint32_t pc = CPU::PC(cpu);
            const ymir_recomp::NativeFn fn = CPU::InDelaySlot(cpu) ? nullptr : table.Find(pc);
            if (CPU::InDelaySlot(cpu)) inSlot++;
            else if (!fn) {
                notFound++;
                if (getenv("FUZZ_TRACE")) printf("  prog %zu: no native code at %08X\n", i, pc);
            }
            if (fn) {
                const uint64_t before = CPU::Cycles(cpu);
                ctx.budget = ~0ull;
                fn(ctx);
                cycles += CPU::Cycles(cpu) - before;
                nativeCalls++;
                if (CPU::PC(cpu) != pc) {
                    steps++;
                    continue;
                }
                noProgress++;
            }
            step();
            interpSteps++;
            steps++;
        }
        const State nat = Snapshot(cpu, cycles);

        // Mixed again, with a tiny cycle budget per native call: native code
        // yields at almost every block start and is re-entered through its
        // switch, interleaved with interpreter steps.
        Load(cpu, p);
        steps = 0;
        cycles = 0;
        while (CPU::PC(cpu) != end && steps < kMaxSteps * 4) {
            const uint32_t pc = CPU::PC(cpu);
            const ymir_recomp::NativeFn fn = CPU::InDelaySlot(cpu) ? nullptr : table.Find(pc);
            if (fn) {
                const uint64_t before = CPU::Cycles(cpu);
                ctx.budget = before + 1 + (steps % 7);
                fn(ctx);
                cycles += CPU::Cycles(cpu) - before;
                yieldRuns++;
                if (CPU::PC(cpu) != pc) {
                    steps++;
                    continue;
                }
            }
            step();
            steps++;
        }
        const State yld = Snapshot(cpu, cycles);

        std::string why;
        if (ref.PC != end) why += "    interpreter did not reach the end\n";
        if (nat.PC != end) why += "    mixed execution did not reach the end\n";
        int d = Compare(ref, nat, why);
        if (yld.PC != end) why += "    yielding execution did not reach the end\n";
        {
            std::string why2;
            const int d2 = Compare(ref, yld, why2);
            if (d2) why += "    [with tiny budget]\n" + why2;
            d += d2;
        }
        if (d || !why.empty()) {
            failed++;
            if (verbose || failed <= 5) {
                printf("program %zu: %d difference(s)\n%s", i, d, why.c_str());
                printf("    code:");
                for (uint32_t k = 0; k < std::min<uint32_t>(p.ncode, kMaxLen); k++) printf(" %04X", p.code[k]);
                printf("\n");
            }
        }
    }
    printf("native function entries: %llu, interpreter steps: %llu (no native code: %llu, delay slot: %llu, "
           "native yielded immediately: %llu)\n",
           (unsigned long long)nativeCalls, (unsigned long long)interpSteps, (unsigned long long)notFound,
           (unsigned long long)inSlot, (unsigned long long)noProgress);
    printf("tiny-budget run: %llu native entries\n", (unsigned long long)yieldRuns);
    printf("%s: %zu programs, %u failed\n", failed ? "FAIL" : "PASS", progs.size(), failed);
    return failed ? 1 : 0;
}
