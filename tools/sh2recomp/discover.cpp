#include "discover.hpp"

#include <ymir/hw/sh2/sh2_decode.hpp>

#include <algorithm>
#include <cstring>

namespace sh2recomp {

using ymir::sh2::DecodeTable;
using ymir::sh2::OpcodeType;

namespace {

    enum : uint8_t { kUnknown = 0, kCode = 1, kData = 2, kConflict = 3 };

    // Known register constants inside one block. `ext`: the value was loaded
    // through the BIOS service table, so a call through it leaves the module.
    struct Regs {
        uint32_t v[16] = {};
        uint16_t known = 0, ext = 0;
        void Clobber(unsigned n) {
            known &= (uint16_t)~(1u << n);
            ext &= (uint16_t)~(1u << n);
        }
        void Set(unsigned n, uint32_t val) {
            v[n] = val;
            known |= (uint16_t)(1u << n);
            ext &= (uint16_t)~(1u << n);
        }
    };

    constexpr uint16_t kCalleeSaved = 0x7F00; // r8-r14
    constexpr uint32_t kBiosTableLo = 0x06000200u, kBiosTableHi = 0x06000400u;

    unsigned N(uint16_t op) { return (op >> 8) & 15; }
    unsigned M(uint16_t op) { return (op >> 4) & 15; }

    enum class Flow { Plain, CondBranch, CondDelayed, Bra, Bsr, Bsrf, Jsr, Jmp, Braf, Rts, Rte, Stop };

    struct Ctx {
        const std::vector<uint8_t> &img;
        uint32_t base, size;
        DiscoverOptions opt;
        std::vector<uint8_t> cls;
        std::vector<uint32_t> visit; // per halfword: function index + 1 that walked it
        std::vector<uint32_t> fnAt;  // per halfword: function index + 1 starting there
        std::vector<uint32_t> fns;
        std::vector<uint8_t> origin;
        size_t next = 0; // first function not yet walked
        DiscoverResult res;

        bool In(uint32_t a, uint32_t len) const { return a >= base && (uint64_t)a - base + len <= size; }
        uint16_t Rd16(uint32_t a) const { return (uint16_t)((img[a - base] << 8) | img[a - base + 1]); }
        uint32_t Rd32(uint32_t a) const {
            const uint8_t *p = &img[a - base];
            return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
        }
        void Mark(uint32_t a, uint32_t len, uint8_t kind) {
            for (uint32_t o = 0; o < len; o += 2) {
                if (!In(a + o, 2)) return;
                uint8_t &c = cls[(a + o - base) >> 1];
                c = (c == kUnknown || c == kind) ? kind : kConflict;
            }
        }
        // Cached (0x0...) and cache-through (0x2...) views alias the same memory.
        bool Norm(uint32_t a, uint32_t &out) const {
            const uint32_t top = a >> 29;
            if (top != 0 && top != 1) return false;
            out = (a & 0x1FFFFFFFu) | (base & 0xE0000000u);
            return true;
        }
        bool AddFn(uint32_t entry, uint8_t from) {
            if ((entry & 1) || !Norm(entry, entry) || !In(entry, 2)) return false;
            uint32_t &slot = fnAt[(entry - base) >> 1];
            if (slot) return true;
            fns.push_back(entry);
            origin.push_back(from);
            slot = (uint32_t)fns.size();
            return true;
        }
    };

    bool Valid(uint16_t op) { return DecodeTable::s_instance.opcodes[0][op] != OpcodeType::Illegal; }

    Flow Classify(uint16_t op, uint32_t pc, uint32_t &target) {
        const int32_t d8 = (int8_t)(op & 0xFF);
        int32_t d12 = op & 0xFFF;
        if (d12 & 0x800) d12 -= 0x1000;
        switch (DecodeTable::s_instance.opcodes[0][op]) {
        case OpcodeType::BT: case OpcodeType::BF: target = pc + 4 + d8 * 2; return Flow::CondBranch;
        case OpcodeType::BTS: case OpcodeType::BFS: target = pc + 4 + d8 * 2; return Flow::CondDelayed;
        case OpcodeType::BRA: target = pc + 4 + d12 * 2; return Flow::Bra;
        case OpcodeType::BSR: target = pc + 4 + d12 * 2; return Flow::Bsr;
        case OpcodeType::BSRF: return Flow::Bsrf;
        case OpcodeType::JSR: return Flow::Jsr;
        case OpcodeType::JMP: return Flow::Jmp;
        case OpcodeType::BRAF: return Flow::Braf;
        case OpcodeType::RTS: return Flow::Rts;
        case OpcodeType::RTE: return Flow::Rte;
        case OpcodeType::Illegal: return Flow::Stop;
        default: return Flow::Plain;
        }
    }

    // Effect of one non-branch instruction on the tracked constants. Anything
    // not understood clobbers every register it might write; over-clobbering
    // only costs resolutions.
    void Track(Ctx &c, uint16_t op, uint32_t pc, Regs &r) {
        const unsigned n = N(op), m = M(op);
        switch (op >> 12) {
        case 0xD: { // mov.l @(disp,PC),Rn
            const uint32_t a = ((pc + 4) & ~3u) + (op & 0xFF) * 4;
            if (c.In(a, 4)) {
                c.Mark(a, 4, kData);
                r.Set(n, c.Rd32(a));
            } else r.Clobber(n);
            return;
        }
        case 0x9: { // mov.w @(disp,PC),Rn
            const uint32_t a = pc + 4 + (op & 0xFF) * 2;
            if (c.In(a, 2)) {
                c.Mark(a, 2, kData);
                r.Set(n, (uint32_t)(int32_t)(int16_t)c.Rd16(a));
            } else r.Clobber(n);
            return;
        }
        case 0xE: r.Set(n, (uint32_t)(int32_t)(int8_t)(op & 0xFF)); return; // mov #imm,Rn
        case 0x1: return;                                                  // mov.l Rm,@(disp,Rn)
        case 0x5: r.Clobber(n); return;                                    // mov.l @(disp,Rm),Rn
        case 0x7: r.Clobber(n); return;                                    // add #imm,Rn
        case 0x8: // r0 forms, compares and branches (branches never get here)
            if ((op & 0x0F00) == 0x0400 || (op & 0x0F00) == 0x0500) r.Clobber(0);
            return;
        case 0xC:
            if ((op & 0xFF00) == 0xC700) { // mova @(disp,PC),R0
                r.Set(0, ((pc + 4) & ~3u) + (op & 0xFF) * 4);
                return;
            }
            switch ((op >> 8) & 15) {
            case 0x4: case 0x5: case 0x6: case 0x9: case 0xA: case 0xB: r.Clobber(0); break;
            default: break; // stores, tst, and @(R0,GBR) memory ops
            }
            return;
        case 0x2:
            switch (op & 15) {
            case 0x4: case 0x5: case 0x6: r.Clobber(n); return; // pre-decrement stores
            case 0x9: case 0xA: case 0xB: case 0xD: r.Clobber(n); return; // and/xor/or/xtrct
            default: return; // stores, tst, cmp/str, div0s, mul
            }
        case 0x3:
            switch (op & 15) {
            case 0x0: case 0x2: case 0x3: case 0x5: case 0x6: case 0x7: case 0xD: return; // cmp, dmul
            default: r.Clobber(n); return;
            }
        case 0x6:
            if ((op & 15) == 0x3) { // mov Rm,Rn
                if (r.known & (1u << m)) r.Set(n, r.v[m]);
                else {
                    const bool ext = (r.ext >> m) & 1;
                    r.Clobber(n);
                    if (ext) r.ext |= (uint16_t)(1u << n);
                }
                return;
            }
            if ((op & 15) == 0x2 && (r.known & (1u << m))) { // mov.l @Rm,Rn through the BIOS table
                const uint32_t a = r.v[m] & 0x1FFFFFFFu;
                if (a >= kBiosTableLo && a < kBiosTableHi) {
                    r.Clobber(n);
                    r.ext |= (uint16_t)(1u << n);
                    return;
                }
            }
            if ((op & 15) >= 0x4 && (op & 15) <= 0x6) r.Clobber(m); // post-increment loads
            r.Clobber(n);
            return;
        case 0x4:
            switch (op & 0xFF) {
            case 0x11: case 0x15: case 0x1B: case 0x0E: case 0x1E: case 0x2E: case 0x0A: case 0x1A: case 0x2A:
                return; // cmp/pz, cmp/pl, tas.b, ldc/lds Rm
            default: break;
            }
            if ((op & 15) == 0xF) r.Clobber(m); // mac.w @Rm+,@Rn+
            r.Clobber(n);
            return;
        case 0x0:
            switch (op & 15) {
            case 0x4: case 0x5: case 0x6: case 0x7: case 0x8: case 0x9: return; // stores, mul.l, flags, nop/div0u
            case 0xF: r.Clobber(m); r.Clobber(n); return;                     // mac.l @Rm+,@Rn+
            default: r.Clobber(n); return;                                    // loads, sts/stc, movt
            }
        default: r.Clobber(n); r.Clobber(0); return;
        }
    }

    // Decodes and marks one instruction. 1: newly walked; 0: this function
    // already walked it; -1: invalid or outside the image.
    int Fetch(Ctx &c, uint32_t fi, uint32_t pc, uint16_t &op) {
        if (!c.In(pc, 2)) return -1;
        op = c.Rd16(pc);
        if (!Valid(op)) return -1;
        uint32_t &v = c.visit[(pc - c.base) >> 1];
        if (v == fi + 1) return 0;
        v = fi + 1;
        c.Mark(pc, 2, kCode);
        return 1;
    }

    struct Block {
        uint32_t addr;
        Regs r;
    };

    void WalkFunction(Ctx &c, uint32_t fi) {
        std::vector<Block> work;
        work.push_back({c.fns[fi], Regs{}});
        auto push = [&](uint32_t a, const Regs &from) {
            Block b{a, Regs{}};
            if (c.opt.assumeABI) {
                b.r = from;
                b.r.known &= kCalleeSaved;
                b.r.ext &= kCalleeSaved;
            }
            work.push_back(b);
        };
        const uint16_t keepAfterCall = c.opt.assumeABI ? kCalleeSaved : 0;

        while (!work.empty()) {
            Block b = work.back();
            work.pop_back();
            uint32_t pc = b.addr;
            Regs r = b.r;
            for (;;) {
                uint16_t op;
                const int st = Fetch(c, fi, pc, op);
                if (st <= 0) break;
                uint32_t target = 0;
                const Flow f = Classify(op, pc, target);
                if (f == Flow::Plain) {
                    Track(c, op, pc, r);
                    pc += 2;
                    continue;
                }
                if (f == Flow::Stop) break;

                // indirect targets are sampled before the delay slot runs
                const unsigned rn = N(op);
                const bool haveTarget = (f == Flow::Jsr || f == Flow::Jmp) && (r.known & (1u << rn));
                const bool viaBios = (f == Flow::Jsr || f == Flow::Jmp) && ((r.ext >> rn) & 1);
                const uint32_t regTarget = r.v[rn];

                uint32_t next = pc + 2;
                if (f != Flow::CondBranch) { // every other transfer has a delay slot
                    uint16_t slot;
                    if (Fetch(c, fi, pc + 2, slot) < 0) break;
                    uint32_t dummy;
                    if (Classify(slot, pc + 2, dummy) != Flow::Plain) break; // illegal slot instruction
                    Track(c, slot, pc + 2, r);
                    next = pc + 4;
                }

                bool fallThrough = false;
                switch (f) {
                case Flow::CondBranch:
                case Flow::CondDelayed:
                    push(target, r);
                    fallThrough = true;
                    break;
                case Flow::Bra: push(target, r); break;
                case Flow::Bsr:
                    c.AddFn(target, 0);
                    r.known &= keepAfterCall;
                    r.ext &= keepAfterCall;
                    fallThrough = true;
                    break;
                case Flow::Jsr:
                    if (haveTarget && c.AddFn(regTarget, 0)) {
                    } else if (viaBios) c.res.biosCalls++;
                    else c.res.unresolvedCalls++;
                    r.known &= keepAfterCall;
                    r.ext &= keepAfterCall;
                    fallThrough = true;
                    break;
                case Flow::Bsrf:
                    c.res.unresolvedCalls++;
                    r.known &= keepAfterCall;
                    r.ext &= keepAfterCall;
                    fallThrough = true;
                    break;
                case Flow::Jmp: // tail call
                    if (haveTarget && c.AddFn(regTarget, 0)) {
                    } else if (viaBios) c.res.biosCalls++;
                    else c.res.unresolvedJumps++;
                    break;
                case Flow::Braf: c.res.unresolvedJumps++; break;
                default: break; // rts, rte
                }
                if (!fallThrough) break;
                pc = next;
            }
        }
    }

    // Code-looking: the first four halfwords are valid instructions and none
    // of them is known data.
    bool LooksLikeCode(const Ctx &c, uint32_t a) {
        for (uint32_t k = 0; k < 4; k++) {
            const uint32_t p = a + 2 * k;
            if (!c.In(p, 2) || c.cls[(p - c.base) >> 1] == kData || !Valid(c.Rd16(p))) return false;
        }
        return true;
    }

    bool IsPrologue(uint16_t op) {
        if (op == 0x4F22) return true;                                                  // sts.l pr,@-r15
        if ((op & 0xFF0F) == 0x2F06 && M(op) >= 8 && M(op) <= 14) return true;         // mov.l r8..r14,@-r15
        return (op & 0xFF00) == 0x7F00 && (op & 0x80);                                  // add #-n,r15
    }

    bool StrongPrologue(const Ctx &c, uint32_t a) {
        if (!c.In(a, 4)) return false;
        const uint16_t op0 = c.Rd16(a), op1 = c.Rd16(a + 2);
        if (op0 == 0x2FE6)
            return op1 == 0x2FD6 || op1 == 0x4F22 || op1 == 0x6EF3 || ((op1 & 0xFF00) == 0x7F00 && (op1 & 0x80));
        if (op0 == 0x4F22) return (op1 & 0xFF00) == 0x7F00 && (op1 & 0x80);
        return false;
    }

    // A weak prologue only starts a function where something ended just before.
    bool PrologueContext(const Ctx &c, uint32_t a) {
        if (StrongPrologue(c, a) || a == c.base) return true;
        const uint8_t prev = c.cls[(a - 2 - c.base) >> 1];
        if (prev == kCode || prev == kData) return true;
        const uint16_t p1 = c.Rd16(a - 2);
        if (p1 == 0x0009 || p1 == 0x0000) return true;             // nop / zero padding
        return a - 4 >= c.base && c.Rd16(a - 4) == 0x000B;          // rts ; slot
    }

} // namespace

DiscoverResult Discover(const std::vector<uint8_t> &img, uint32_t base, const std::vector<uint32_t> &seeds,
                        const DiscoverOptions &opt) {
    Ctx c{img, base, (uint32_t)(img.size() & ~size_t(1)), opt, {}, {}, {}, {}, {}, 0, {}};
    const size_t halfwords = c.size / 2;
    c.cls.assign(halfwords, kUnknown);
    c.visit.assign(halfwords, 0);
    c.fnAt.assign(halfwords, 0);

    for (uint32_t s : seeds) c.AddFn(s, 0);

    for (;;) {
        while (c.next < c.fns.size()) {
            WalkFunction(c, (uint32_t)c.next);
            c.next++;
        }
        size_t added = 0;
        if (opt.poolPointers) {
            for (size_t h = 0; h + 1 < halfwords; h += 2) {
                if (c.cls[h] != kData || c.cls[h + 1] != kData) continue;
                const uint32_t a = base + (uint32_t)(h * 2);
                uint32_t v = c.Rd32(a);
                if ((v & 1) || !c.Norm(v, v) || !c.In(v, 2)) continue;
                if (c.cls[(v - base) >> 1] != kUnknown || !LooksLikeCode(c, v)) continue;
                if (c.fnAt[(v - base) >> 1]) continue;
                if (c.AddFn(v, 1)) added++;
            }
        }
        if (added) continue;
        // Prologues last, one at a time, so each new function's pools are
        // marked as data before the next candidate is tested.
        if (opt.prologues) {
            for (size_t h = 1; h < halfwords; h++) {
                const uint32_t a = base + (uint32_t)(h * 2);
                if (c.cls[h] != kUnknown || c.fnAt[h] || !IsPrologue(c.Rd16(a))) continue;
                if (!PrologueContext(c, a) || !LooksLikeCode(c, a)) continue;
                if (c.AddFn(a, 2)) {
                    added++;
                    break;
                }
            }
        }
        if (!added) break;
    }

    DiscoverResult &res = c.res;
    res.entries = c.fns;
    std::sort(res.entries.begin(), res.entries.end());
    for (uint8_t o : c.origin) (o == 0 ? res.fromFlow : o == 1 ? res.fromPool : res.fromPrologue)++;
    for (uint8_t k : c.cls) {
        if (k == kCode) res.codeBytes += 2;
        else if (k == kData) res.dataBytes += 2;
    }
    return res;
}

} // namespace sh2recomp
