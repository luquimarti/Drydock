// sh2fuzz-gen <seed> <count> <len> <out.bin> <out.cpp>
//
// Generates random straight-line SH-2 programs and the C++ that sh2gen emits
// for each of them. Memory instructions only use dedicated base registers that
// point into a 4 KiB RAM window, so every access is valid, aligned RAM:
//   r8  byte base      r9, r12  word bases      r10, r11  long bases
//   GBR long-aligned base; @(R0,..) forms are preceded by and #0x3C,r0.
// No other instruction writes r8-r12 or GBR.

#include "fuzz_common.hpp"

#include "../../tools/sh2gen/emit.hpp"

#include <algorithm>
#include <cstdlib>
#include <random>
#include <string>

using namespace fuzz;

namespace {

std::mt19937 rng;
uint32_t Rnd(uint32_t n) { return std::uniform_int_distribution<uint32_t>(0, n - 1)(rng); }

const unsigned kDst[] = {0, 1, 2, 3, 4, 5, 6, 7, 13, 14, 15};
unsigned Dst() { return kDst[Rnd(11)]; }
unsigned Any() { return Rnd(16); }
unsigned ByteBase() { const unsigned b[] = {8, 9, 10, 11, 12}; return b[Rnd(5)]; }
unsigned WordBase() { const unsigned b[] = {9, 10, 11, 12}; return b[Rnd(4)]; }
unsigned LongBase() { return 10 + Rnd(2); }
unsigned WordIncBase() { return Rnd(2) ? 9 : 12; }

uint16_t NM(uint16_t hi, unsigned n, unsigned m, uint16_t lo) { return (uint16_t)((hi << 12) | (n << 8) | (m << 4) | lo); }

// Appends one instruction (sometimes preceded by and #0x3C,r0).
void Emit(std::vector<uint16_t> &c) {
    const uint16_t andR0 = 0xC93C;
    switch (Rnd(14)) {
    case 0: case 1: case 2: { // two-register ALU
        static const uint16_t ops[][2] = {
            {3, 0xC}, {3, 0xE}, {3, 0xF}, {3, 0x8}, {3, 0xA}, {3, 0xB}, {2, 0x9}, {2, 0xB}, {2, 0xA}, {2, 0x8},
            {3, 0x0}, {3, 0x3}, {3, 0x7}, {3, 0x6}, {3, 0x2}, {2, 0xC}, {6, 0x3}, {6, 0xB}, {6, 0xA}, {6, 0x7},
            {6, 0xC}, {6, 0xD}, {6, 0xE}, {6, 0xF}, {6, 0x8}, {6, 0x9}, {2, 0xD}, {0, 0x7}, {2, 0xF}, {2, 0xE},
            {3, 0xD}, {3, 0x5}, {2, 0x7}, {3, 0x4}, {3, 0x4}, {3, 0x4}};
        const auto &o = ops[Rnd(sizeof ops / sizeof ops[0])];
        c.push_back(NM(o[0], Dst(), Any(), o[1]));
        break;
    }
    case 3: { // one-register shifts/rotates/tests
        static const uint16_t lo[] = {0x04, 0x05, 0x24, 0x25, 0x20, 0x21, 0x00, 0x01, 0x08,
                                      0x09, 0x18, 0x19, 0x28, 0x29, 0x10, 0x15, 0x11};
        c.push_back((uint16_t)(0x4000 | (Dst() << 8) | lo[Rnd(sizeof lo / sizeof lo[0])]));
        break;
    }
    case 4: { // immediates and zero-operand
        switch (Rnd(10)) {
        case 0: c.push_back((uint16_t)(0xE000 | (Dst() << 8) | Rnd(256))); break; // mov #i
        case 1: c.push_back((uint16_t)(0x7000 | (Dst() << 8) | Rnd(256))); break; // add #i
        case 2: c.push_back((uint16_t)(0xC900 | Rnd(256))); break;                 // and #i,r0
        case 3: c.push_back((uint16_t)(0xCB00 | Rnd(256))); break;                 // or
        case 4: c.push_back((uint16_t)(0xCA00 | Rnd(256))); break;                 // xor
        case 5: c.push_back((uint16_t)(0xC800 | Rnd(256))); break;                 // tst
        case 6: c.push_back((uint16_t)(0x8800 | Rnd(256))); break;                 // cmp/eq
        case 7: { const uint16_t z[] = {0x0009, 0x0008, 0x0018, 0x0028, 0x0019}; c.push_back(z[Rnd(5)]); break; }
        case 8: c.push_back((uint16_t)(0x0029 | (Dst() << 8))); break;             // movt
        default: c.push_back((uint16_t)(0x6003 | (Dst() << 8) | (Any() << 4))); break;
        }
        break;
    }
    case 5: { // system registers
        static const uint16_t ld[] = {0x2E, 0x0A, 0x1A, 0x2A};       // ldc vbr, lds mach/macl/pr
        static const uint16_t st[] = {0x12, 0x02, 0x22, 0x0A, 0x1A, 0x2A}; // stc gbr/sr/vbr, sts ...
        if (Rnd(2)) c.push_back((uint16_t)(0x4000 | (Any() << 8) | ld[Rnd(4)]));
        else c.push_back((uint16_t)(0x0000 | (Dst() << 8) | st[Rnd(6)]));
        break;
    }
    case 6: { // loads @Rm, @Rm+
        const unsigned sz = Rnd(3);
        const bool inc = Rnd(2);
        unsigned m = sz == 0 ? (inc ? 8 : ByteBase()) : sz == 1 ? (inc ? WordIncBase() : WordBase()) : LongBase();
        c.push_back(NM(6, Dst(), m, (uint16_t)((inc ? 4 : 0) + sz)));
        break;
    }
    case 7: { // loads @(R0,Rm), @(disp,Rm), @(disp,GBR)
        const unsigned sz = Rnd(3);
        switch (Rnd(3)) {
        case 0:
            c.push_back(andR0);
            c.push_back(NM(0, Dst(), sz == 0 ? ByteBase() : sz == 1 ? WordBase() : LongBase(), (uint16_t)(0xC + sz)));
            break;
        case 1:
            if (sz == 0) c.push_back((uint16_t)(0x8400 | (ByteBase() << 4) | Rnd(16)));
            else if (sz == 1) c.push_back((uint16_t)(0x8500 | (WordBase() << 4) | Rnd(16)));
            else c.push_back(NM(5, Dst(), LongBase(), (uint16_t)Rnd(16)));
            break;
        default: c.push_back((uint16_t)(0xC400 | (sz << 8) | Rnd(256))); break;
        }
        break;
    }
    case 8: { // PC-relative
        switch (Rnd(3)) {
        case 0: c.push_back((uint16_t)(0x9000 | (Dst() << 8) | Rnd(256))); break;
        case 1: c.push_back((uint16_t)(0xD000 | (Dst() << 8) | Rnd(256))); break;
        default: c.push_back((uint16_t)(0xC700 | Rnd(256))); break;
        }
        break;
    }
    case 9: { // stores @Rn, @-Rn
        const unsigned sz = Rnd(3);
        const bool dec = Rnd(2);
        unsigned n = sz == 0 ? (dec ? 8 : ByteBase()) : sz == 1 ? (dec ? WordIncBase() : WordBase()) : LongBase();
        c.push_back(NM(2, n, Any(), (uint16_t)((dec ? 4 : 0) + sz)));
        break;
    }
    case 10: { // stores @(R0,Rn), @(disp,Rn), @(disp,GBR)
        const unsigned sz = Rnd(3);
        switch (Rnd(3)) {
        case 0:
            c.push_back(andR0);
            c.push_back(NM(0, sz == 0 ? ByteBase() : sz == 1 ? WordBase() : LongBase(), Any(), (uint16_t)(0x4 + sz)));
            break;
        case 1:
            if (sz == 0) c.push_back((uint16_t)(0x8000 | (ByteBase() << 4) | Rnd(16)));
            else if (sz == 1) c.push_back((uint16_t)(0x8100 | (WordBase() << 4) | Rnd(16)));
            else c.push_back(NM(1, LongBase(), Any(), (uint16_t)Rnd(16)));
            break;
        default: c.push_back((uint16_t)(0xC000 | (sz << 8) | Rnd(256))); break;
        }
        break;
    }
    case 11: { // read-modify-write on @(R0,GBR), tas.b
        if (Rnd(5) == 0) {
            c.push_back(0x481B); // tas.b @r8
        } else {
            static const uint16_t hi[] = {0xCD00, 0xCF00, 0xCE00, 0xCC00};
            c.push_back(andR0);
            c.push_back((uint16_t)(hi[Rnd(4)] | Rnd(256)));
        }
        break;
    }
    case 12: { // mac.w / mac.l
        if (Rnd(2)) c.push_back(NM(4, 12, 9, 0xF));       // mac.w @r9+,@r12+
        else c.push_back(NM(0, 11, 10, 0xF));             // mac.l @r10+,@r11+
        break;
    }
    default: { // ld.l @Rm+,x / st.l x,@-Rn on long bases
        static const uint16_t ld[] = {0x27, 0x06, 0x16, 0x26};
        static const uint16_t st[] = {0x13, 0x03, 0x23, 0x02, 0x12, 0x22};
        if (Rnd(2)) c.push_back((uint16_t)(0x4000 | (LongBase() << 8) | ld[Rnd(4)]));
        else c.push_back((uint16_t)(0x4000 | (LongBase() << 8) | st[Rnd(6)]));
        break;
    }
    }
}

// One straight-line instruction (never the 'and #0x3C,r0' + op pairs), for
// delay slots. PR writers are optional so subroutines can keep their return
// address.
uint16_t One(bool allowPRWrite) {
    for (;;) {
        std::vector<uint16_t> v;
        Emit(v);
        if (v.size() != 1) continue;
        const uint16_t op = v[0];
        if (!allowPRWrite && ((op & 0xF0FF) == 0x402A || (op & 0xF0FF) == 0x4026)) continue;
        return op;
    }
}

void Straight(std::vector<uint16_t> &c, bool allowPRWrite) {
    for (;;) {
        std::vector<uint16_t> v;
        Emit(v);
        bool ok = true;
        if (!allowPRWrite)
            for (uint16_t op : v)
                if ((op & 0xF0FF) == 0x402A || (op & 0xF0FF) == 0x4026) ok = false;
        if (!ok) continue;
        c.insert(c.end(), v.begin(), v.end());
        return;
    }
}

// ---- tiny assembler with forward references -----------------------------------

enum class Fix { None, Br8, Br12, Pool };
struct Ins {
    uint16_t op;
    Fix fix = Fix::None;
    int label = -1;    // Br8/Br12: label id; Pool: pool slot id
};
struct PoolSlot {
    bool relative;     // braf/bsrf offset instead of absolute address
    int label;
    int branchIns;     // index of the braf/bsrf instruction (for relative)
};

struct Asm {
    std::vector<Ins> ins;
    std::vector<int> labelIns; // label -> instruction index (-1: unresolved)
    std::vector<PoolSlot> pool;

    int NewLabel() { labelIns.push_back(-1); return (int)labelIns.size() - 1; }
    void Bind(int l) { labelIns[l] = (int)ins.size(); }
    void Op(uint16_t op) { ins.push_back({op}); }
    void Br(uint16_t op, Fix f, int l) { ins.push_back({op, f, l}); }
};

} // namespace

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: %s seed count len out.bin out.cpp\n", argv[0]);
        return 2;
    }
    rng.seed((uint32_t)strtoul(argv[1], nullptr, 0));
    const uint32_t count = (uint32_t)strtoul(argv[2], nullptr, 0);
    const uint32_t items = (uint32_t)strtoul(argv[3], nullptr, 0);

    std::vector<Program> progs(count);
    std::string cpp = sh2gen::FilePrologue();
    cpp += "#include <ymir_recomp/dispatch.hpp>\n\nnamespace fuzz_gen {\n\n";
    std::string regs;
    uint64_t totalIns = 0, totalExits = 0;

    for (uint32_t pi = 0; pi < count; pi++) {
        Program &p = progs[pi];
        for (auto &b : p.codeRegion) b = (uint8_t)Rnd(256);
        for (auto &b : p.window) b = (uint8_t)Rnd(256);
        for (auto &r : p.R) r = rng();
        p.R[8] = kWindowBase + 0x400 + Rnd(4);
        p.R[9] = kWindowBase + 0x800 + 2 * Rnd(8);
        p.R[12] = kWindowBase + 0xA00 + 2 * Rnd(8);
        p.R[10] = kWindowBase + 0xC00 + 4 * Rnd(16);
        p.R[11] = kWindowBase + 0x600 + 4 * Rnd(16);
        p.GBR = kWindowBase + 4 * Rnd(32);
        p.VBR = rng();
        p.PR = rng();
        p.MAC = ((uint64_t)rng() << 32) | rng();
        p.SR = (Rnd(2) << 0) | (Rnd(2) << 1) | (0xFu << 4) | (Rnd(2) << 8) | (Rnd(2) << 9);

        // ---- main body: straight code with forward branches and calls ----
        const int kSubs = 3;
        Asm a;
        std::vector<int> itemLabel(items + 1);
        for (auto &l : itemLabel) l = a.NewLabel();
        std::vector<int> subLabel(kSubs);
        for (auto &l : subLabel) l = a.NewLabel();
        const int endLabel = itemLabel[items];

        auto fwd = [&](uint32_t i) { return itemLabel[i + 1 + Rnd(items - i)]; }; // later item or end
        for (uint32_t i = 0; i < items; i++) {
            a.Bind(itemLabel[i]);
            const uint32_t k = Rnd(16);
            if (k < 10) {
                std::vector<uint16_t> v;
                Straight(v, true);
                for (uint16_t op : v) a.Op(op);
            } else if (k == 10) { // bt / bf
                a.Br(Rnd(2) ? 0x8900 : 0x8B00, Fix::Br8, fwd(i));
            } else if (k == 11) { // bt/s / bf/s + slot
                a.Br(Rnd(2) ? 0x8D00 : 0x8F00, Fix::Br8, fwd(i));
                a.Op(One(true));
            } else if (k == 12) { // bra + slot
                a.Br(0xA000, Fix::Br12, fwd(i));
                a.Op(One(true));
            } else if (k == 13) { // bsr sub + slot (the slot must not overwrite PR)
                a.Br(0xB000, Fix::Br12, subLabel[Rnd(kSubs)]);
                a.Op(One(false));
            } else { // computed: jsr/bsrf to a sub, jmp/braf forward, via a pool constant in r13
                const uint32_t which = Rnd(4);
                const bool call = which < 2, rel = (which & 1) != 0;
                const int target = call ? subLabel[Rnd(kSubs)] : fwd(i);
                a.pool.push_back({rel, target, (int)a.ins.size() + 1});
                a.Br(0xDD00, Fix::Pool, (int)a.pool.size() - 1); // mov.l @(pool),r13
                const uint16_t op = call ? (rel ? 0x0D03 : 0x4D0B) : (rel ? 0x0D23 : 0x4D2B);
                a.Op(op);
                // the target is sampled before the slot, so the slot may freely
                // overwrite r13; a call's slot must not overwrite PR
                a.Op(One(!call));
            }
        }
        a.Bind(endLabel);
        const uint32_t mainLen = (uint32_t)a.ins.size();

        // ---- subroutines: straight code, rts + slot; leave a gap after main ----
        const uint32_t gapIns = 8;
        std::vector<std::pair<uint32_t, uint32_t>> segments; // [start,end) in instruction indices
        segments.push_back({0, mainLen});
        for (int s = 0; s < kSubs; s++) {
            for (uint32_t g = 0; g < gapIns; g++) a.Op(0xFFFF); // never read as code
            const uint32_t begin = (uint32_t)a.ins.size();
            a.Bind(subLabel[s]);
            const uint32_t n = 2 + Rnd(10);
            for (uint32_t k = 0; k < n; k++) {
                std::vector<uint16_t> v;
                Straight(v, false);
                for (uint16_t op : v) a.Op(op);
            }
            a.Op(0x000B);       // rts
            a.Op(One(false));   // slot
            segments.push_back({begin, (uint32_t)a.ins.size()});
        }

        // ---- layout and fixups ----
        auto addrOf = [&](uint32_t insIndex) { return kCodeBase + insIndex * 2; };
        uint32_t poolBase = (addrOf((uint32_t)a.ins.size()) + 3) & ~3u;
        bool bad = false;
        std::vector<uint16_t> image(a.ins.size());
        for (size_t k = 0; k < a.ins.size(); k++) {
            Ins in = a.ins[k];
            const uint32_t pc = addrOf((uint32_t)k);
            if (in.fix == Fix::Br8 || in.fix == Fix::Br12) {
                const int32_t disp = ((int32_t)addrOf((uint32_t)a.labelIns[in.label]) - (int32_t)(pc + 4)) / 2;
                const int32_t lim = in.fix == Fix::Br8 ? 128 : 2048;
                if (disp < -lim || disp >= lim) bad = true;
                in.op |= (uint16_t)(disp & (in.fix == Fix::Br8 ? 0xFF : 0xFFF));
            } else if (in.fix == Fix::Pool) {
                const uint32_t slotAddr = poolBase + in.label * 4;
                const uint32_t d = (slotAddr - ((pc & ~3u) + 4)) / 4;
                if (d > 255) bad = true;
                in.op |= (uint16_t)d;
            }
            image[k] = in.op;
        }
        if (bad || a.ins.size() * 2 + a.pool.size() * 4 + 4 > kRegionSize - 64) {
            pi--; // retry with another random program
            continue;
        }
        for (size_t k = 0; k < image.size(); k++) {
            p.codeRegion[k * 2] = (uint8_t)(image[k] >> 8);
            p.codeRegion[k * 2 + 1] = (uint8_t)image[k];
        }
        for (size_t s = 0; s < a.pool.size(); s++) {
            const PoolSlot &ps = a.pool[s];
            const uint32_t target = addrOf((uint32_t)a.labelIns[ps.label]);
            const uint32_t v = ps.relative ? target - (addrOf((uint32_t)ps.branchIns) + 4) : target;
            const uint32_t off = poolBase - kCodeBase + (uint32_t)s * 4;
            p.codeRegion[off] = (uint8_t)(v >> 24);
            p.codeRegion[off + 1] = (uint8_t)(v >> 16);
            p.codeRegion[off + 2] = (uint8_t)(v >> 8);
            p.codeRegion[off + 3] = (uint8_t)v;
        }
        p.ncode = mainLen; // the program ends when PC reaches the end of main
        for (uint32_t k = 0; k < std::min<uint32_t>(mainLen, kMaxLen); k++) p.code[k] = image[k];

        // ---- emit native code for main and every subroutine ----
        auto reader = [&](uint32_t addr, uint16_t &op) {
            if (addr < kCodeBase || (addr & 1)) return false;
            const uint32_t idx = (addr - kCodeBase) / 2;
            for (auto &sg : segments)
                if (idx >= sg.first && idx < sg.second) {
                    op = image[idx];
                    return true;
                }
            return false;
        };
        std::vector<uint32_t> entries{kCodeBase};
        for (int s = 0; s < kSubs; s++) entries.push_back(addrOf((uint32_t)a.labelIns[subLabel[s]]));
        regs += "void register" + std::to_string(pi) + "(ymir_recomp::Table &t) {\n";
        for (uint32_t e : entries) {
            char name[64];
            snprintf(name, sizeof name, "p%u_f%08X", pi, e);
            // Main also gets every item start as an extra entry: what a runtime
            // call log would provide for the braf/jmp targets.
            std::vector<uint32_t> extra;
            if (e == kCodeBase)
                for (uint32_t it = 0; it < items; it++) extra.push_back(addrOf((uint32_t)a.labelIns[itemLabel[it]]));
            const sh2gen::FunctionInfo fi = sh2gen::EmitFunction(name, e, reader, cpp, extra);
            cpp += "\n";
            totalIns += fi.instructions;
            totalExits += fi.interpreterExits;
            for (uint32_t l : fi.labels) {
                char line[96];
                snprintf(line, sizeof line, "    t.Add(0x%08Xu, %s);\n", l, name);
                regs += line;
            }
        }
        regs += "}\n";
    }
    cpp += regs;
    cpp += "using RegFn = void (*)(ymir_recomp::Table &);\n";
    cpp += "extern const RegFn g_register[] = {\n";
    for (uint32_t i = 0; i < count; i++) cpp += "    register" + std::to_string(i) + ",\n";
    cpp += "};\nextern const unsigned g_programCount = " + std::to_string(count) + ";\n\n} // namespace fuzz_gen\n";

    if (!WritePrograms(argv[4], progs)) { fprintf(stderr, "cannot write %s\n", argv[4]); return 1; }
    FILE *f = fopen(argv[5], "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", argv[5]); return 1; }
    fwrite(cpp.data(), 1, cpp.size(), f);
    fclose(f);
    printf("generated %u programs (%u items each): %llu instructions compiled, %llu interpreter exits\n", count, items,
           (unsigned long long)totalIns, (unsigned long long)totalExits);
    return 0;
}
