#include "emit.hpp"

#include <ymir/hw/sh2/sh2_decode.hpp>

#include <algorithm>
#include <cstdarg>
#include <map>
#include <set>
#include <cstdio>
#include <string>

namespace sh2gen {

using ymir::sh2::DecodeTable;
using ymir::sh2::OpcodeType;

namespace {

    std::string Fmt(const char *fmt, ...) {
        va_list ap, ap2;
        va_start(ap, fmt);
        va_copy(ap2, ap);
        const int len = vsnprintf(nullptr, 0, fmt, ap);
        va_end(ap);
        std::string out((size_t)len, '\0');
        vsnprintf(out.data(), (size_t)len + 1, fmt, ap2);
        va_end(ap2);
        return out;
    }

    // Operand fields.
    unsigned N(uint16_t op) { return (op >> 8) & 15; }  // bits 8-11
    unsigned M(uint16_t op) { return (op >> 4) & 15; }  // bits 4-7
    unsigned D4(uint16_t op) { return op & 15; }
    unsigned U8(uint16_t op) { return op & 0xFF; }
    int32_t S8(uint16_t op) { return (int8_t)(op & 0xFF); }

    // Memory access snippets. Values are always held in uint32_t.
    std::string Rd(unsigned size, const std::string &addr, bool sext) {
        switch (size) {
        case 1: return sext ? "(uint32_t)(int32_t)(int8_t)cpu.RecompRead8C(" + addr + ", cyc)"
                            : "(uint32_t)cpu.RecompRead8C(" + addr + ", cyc)";
        case 2: return sext ? "(uint32_t)(int32_t)(int16_t)cpu.RecompRead16C(" + addr + ", cyc)"
                            : "(uint32_t)cpu.RecompRead16C(" + addr + ", cyc)";
        default: return "cpu.RecompRead32C(" + addr + ", cyc)";
        }
    }
    std::string Wr(unsigned size, const std::string &addr, const std::string &val) {
        switch (size) {
        case 1: return "cpu.RecompWrite8C(" + addr + ", (uint8_t)(" + val + "), cyc);";
        case 2: return "cpu.RecompWrite16C(" + addr + ", (uint16_t)(" + val + "), cyc);";
        default: return "cpu.RecompWrite32C(" + addr + ", " + val + ", cyc);";
        }
    }
    const char *Sfx(unsigned size) { return size == 1 ? "b" : size == 2 ? "w" : "l"; }

} // namespace


// Cycle model of Ymir's interpreter for one supported instruction: the fixed
// cost (memory access cycles are added at run time by the accessors), the
// registers whose pending load stalls it, and the load it leaves pending.
// Mirrors libs/ymir-core/src/ymir/hw/sh2/sh2.cpp, quirks included.
static void ApplyCycleModel(Emitted &e, OpcodeType t, uint16_t op) {
    const uint8_t n = (uint8_t)N(op), m = (uint8_t)M(op);
    constexpr uint8_t PR = 0x10, None = 0xFF;
    auto set = [&](uint32_t base, std::vector<uint8_t> rd, uint8_t wb = None) {
        e.cycles = base;
        e.wbRead = std::move(rd);
        e.wbSet = wb;
    };
    switch (t) {
    case OpcodeType::NOP: case OpcodeType::CLRT: case OpcodeType::SETT: case OpcodeType::CLRMAC:
    case OpcodeType::DIV0U: set(1, {}); break;
    case OpcodeType::MOV_R: case OpcodeType::EXTUB: case OpcodeType::EXTUW: case OpcodeType::EXTSB:
    case OpcodeType::EXTSW: case OpcodeType::SWAPB: case OpcodeType::SWAPW: case OpcodeType::XTRCT:
    case OpcodeType::ADD: case OpcodeType::ADDC: case OpcodeType::ADDV: case OpcodeType::SUB:
    case OpcodeType::SUBC: case OpcodeType::SUBV: case OpcodeType::NEG: case OpcodeType::NEGC:
    case OpcodeType::AND_R: case OpcodeType::OR_R: case OpcodeType::XOR_R: case OpcodeType::NOT:
    case OpcodeType::MULS: case OpcodeType::MULU: case OpcodeType::DIV0S: case OpcodeType::DIV1:
    case OpcodeType::CMP_EQ_R: case OpcodeType::CMP_GE: case OpcodeType::CMP_GT: case OpcodeType::CMP_HI:
    case OpcodeType::CMP_HS: case OpcodeType::CMP_STR: case OpcodeType::TST_R: set(1, {m, n}); break;
    case OpcodeType::MUL: case OpcodeType::DMULS: case OpcodeType::DMULU: set(3, {m, n}); break;
    case OpcodeType::MOV_I: case OpcodeType::MOVT: case OpcodeType::ADD_I: case OpcodeType::DT:
    case OpcodeType::CMP_PL: case OpcodeType::CMP_PZ: case OpcodeType::ROTL: case OpcodeType::ROTR:
    case OpcodeType::ROTCL: case OpcodeType::ROTCR: case OpcodeType::SHAL: case OpcodeType::SHAR:
    case OpcodeType::SHLL: case OpcodeType::SHLR: case OpcodeType::SHLL2: case OpcodeType::SHLL8:
    case OpcodeType::SHLL16: case OpcodeType::SHLR2: case OpcodeType::SHLR8: case OpcodeType::SHLR16:
        set(1, {n}); break;
    case OpcodeType::MOVA: case OpcodeType::AND_I: case OpcodeType::OR_I: case OpcodeType::XOR_I:
    case OpcodeType::TST_I: case OpcodeType::CMP_EQ_I: set(1, {0}); break;

    // loads: access cycles only
    case OpcodeType::MOVB_L: case OpcodeType::MOVW_L: case OpcodeType::MOVL_L:
    case OpcodeType::MOVB_P: case OpcodeType::MOVW_P: case OpcodeType::MOVL_P: set(0, {m}, n); break;
    case OpcodeType::MOVB_L0: case OpcodeType::MOVW_L0: case OpcodeType::MOVL_L0: set(0, {m, 0}, n); break;
    case OpcodeType::MOVB_L4: set(0, {}, 0); break;
    case OpcodeType::MOVW_L4: set(0, {m}, 0); e.wbTwice = true; break; // Ymir adds this stall twice
    case OpcodeType::MOVL_L4: set(0, {m}, n); break;
    case OpcodeType::MOVB_LG: case OpcodeType::MOVW_LG: case OpcodeType::MOVL_LG: set(0, {}, 0); break;
    case OpcodeType::MOVW_I: case OpcodeType::MOVL_I: set(0, {}, n); break;

    // stores
    case OpcodeType::MOVB_M: case OpcodeType::MOVW_M: case OpcodeType::MOVL_M:
    case OpcodeType::MOVB_S: case OpcodeType::MOVW_S: case OpcodeType::MOVL_S:
    case OpcodeType::MOVL_S4: set(0, {m, n}); break;
    case OpcodeType::MOVB_S0: case OpcodeType::MOVW_S0: case OpcodeType::MOVL_S0: set(0, {n, 0}); break;
    case OpcodeType::MOVB_S4: case OpcodeType::MOVW_S4: set(0, {m, 0}); break; // nd4: base register in bits 4-7
    case OpcodeType::MOVB_SG: case OpcodeType::MOVW_SG: case OpcodeType::MOVL_SG: set(0, {0}); break;

    // system registers (the register operand is in bits 8-11)
    case OpcodeType::LDC_GBR_R: case OpcodeType::LDC_VBR_R: case OpcodeType::LDS_MACH_R:
    case OpcodeType::LDS_MACL_R: case OpcodeType::STC_GBR_R: case OpcodeType::STC_SR_R:
    case OpcodeType::STC_VBR_R: set(1, {n}); break;
    case OpcodeType::LDS_PR_R: case OpcodeType::STS_PR_R: set(1, {n, PR}); break;
    case OpcodeType::STS_MACH_R: case OpcodeType::STS_MACL_R: set(1, {}, n); break;
    case OpcodeType::LDC_GBR_M: case OpcodeType::LDC_VBR_M: set(2, {n}); break;
    case OpcodeType::LDS_MACH_M: case OpcodeType::LDS_MACL_M: set(0, {n}); break;
    case OpcodeType::LDS_PR_M: set(0, {n}, PR); break;
    case OpcodeType::STC_GBR_M: case OpcodeType::STC_SR_M: case OpcodeType::STC_VBR_M: set(2, {n}); break;
    case OpcodeType::STS_MACH_M: case OpcodeType::STS_MACL_M: set(0, {n}); break;
    case OpcodeType::STS_PR_M: set(0, {n, PR}); break;

    // memory arithmetic
    case OpcodeType::MACW: case OpcodeType::MACL: set(1, {m, n}); break;
    case OpcodeType::AND_M: case OpcodeType::OR_M: case OpcodeType::XOR_M: set(1, {0}); break;
    case OpcodeType::TST_M: set(2, {0}); break;
    case OpcodeType::TAS: set(4, {n}); break;
    default: break;
    }
}

Emitted EmitInstruction(uint16_t op, uint32_t pc, const char *slotPC) {
    Emitted e;
    const OpcodeType t = DecodeTable::s_instance.opcodes[0][op];
    // Inside a taken delay slot Ymir evaluates PC-relative operands against
    // (branch target - 2), which may only be known at run time.
    const std::string pcExpr = slotPC ? std::string("(") + slotPC + ")" : Fmt("0x%08Xu", pc);
    const unsigned n = N(op), m = M(op);
    std::string &c = e.code;

    auto unsupported = [&](const char *name) {
        e.status = EmitStatus::Unsupported;
        e.mnemonic = name;
        return e;
    };

    switch (t) {
    // ---- data transfer --------------------------------------------------
    case OpcodeType::NOP: e.mnemonic = "nop"; break;
    case OpcodeType::MOV_R:
        e.mnemonic = Fmt("mov r%u,r%u", m, n);
        c = Fmt("R[%u] = R[%u];", n, m);
        break;

    case OpcodeType::MOVB_L:
    case OpcodeType::MOVW_L:
    case OpcodeType::MOVL_L: {
        const unsigned sz = t == OpcodeType::MOVB_L ? 1 : t == OpcodeType::MOVW_L ? 2 : 4;
        e.mnemonic = Fmt("mov.%s @r%u,r%u", Sfx(sz), m, n);
        c = Fmt("R[%u] = %s;", n, Rd(sz, Fmt("R[%u]", m), true).c_str());
        break;
    }
    case OpcodeType::MOVB_L0:
    case OpcodeType::MOVW_L0:
    case OpcodeType::MOVL_L0: {
        const unsigned sz = t == OpcodeType::MOVB_L0 ? 1 : t == OpcodeType::MOVW_L0 ? 2 : 4;
        e.mnemonic = Fmt("mov.%s @(r0,r%u),r%u", Sfx(sz), m, n);
        c = Fmt("R[%u] = %s;", n, Rd(sz, Fmt("R[0] + R[%u]", m), true).c_str());
        break;
    }
    case OpcodeType::MOVB_L4:
    case OpcodeType::MOVW_L4: {
        const unsigned sz = t == OpcodeType::MOVB_L4 ? 1 : 2;
        const unsigned d = D4(op) * sz;
        e.mnemonic = Fmt("mov.%s @(%u,r%u),r0", Sfx(sz), d, m);
        c = Fmt("R[0] = %s;", Rd(sz, Fmt("R[%u] + %uu", m, d), true).c_str());
        break;
    }
    case OpcodeType::MOVL_L4: {
        const unsigned d = D4(op) * 4;
        e.mnemonic = Fmt("mov.l @(%u,r%u),r%u", d, m, n);
        c = Fmt("R[%u] = %s;", n, Rd(4, Fmt("R[%u] + %uu", m, d), true).c_str());
        break;
    }
    case OpcodeType::MOVB_LG:
    case OpcodeType::MOVW_LG:
    case OpcodeType::MOVL_LG: {
        const unsigned sz = t == OpcodeType::MOVB_LG ? 1 : t == OpcodeType::MOVW_LG ? 2 : 4;
        const unsigned d = U8(op) * sz;
        e.mnemonic = Fmt("mov.%s @(%u,gbr),r0", Sfx(sz), d);
        c = Fmt("R[0] = %s;", Rd(sz, Fmt("GBR + %uu", d), true).c_str());
        break;
    }
    case OpcodeType::MOVB_M:
    case OpcodeType::MOVW_M:
    case OpcodeType::MOVL_M: {
        const unsigned sz = t == OpcodeType::MOVB_M ? 1 : t == OpcodeType::MOVW_M ? 2 : 4;
        e.mnemonic = Fmt("mov.%s r%u,@-r%u", Sfx(sz), m, n);
        // Ymir: address = Rn - size; write Rm (read before Rn changes); Rn = address
        c = Fmt("{ const uint32_t a = R[%u] - %uu; %s R[%u] = a; }", n, sz,
                Wr(sz, "a", Fmt("R[%u]", m)).c_str(), n);
        break;
    }
    case OpcodeType::MOVB_P:
    case OpcodeType::MOVW_P:
    case OpcodeType::MOVL_P: {
        const unsigned sz = t == OpcodeType::MOVB_P ? 1 : t == OpcodeType::MOVW_P ? 2 : 4;
        e.mnemonic = Fmt("mov.%s @r%u+,r%u", Sfx(sz), m, n);
        // Ymir: Rn = load(Rm); if (n != m) Rm += size
        c = Fmt("R[%u] = %s;", n, Rd(sz, Fmt("R[%u]", m), true).c_str());
        if (n != m) c += Fmt(" R[%u] += %uu;", m, sz);
        break;
    }
    case OpcodeType::MOVB_S:
    case OpcodeType::MOVW_S:
    case OpcodeType::MOVL_S: {
        const unsigned sz = t == OpcodeType::MOVB_S ? 1 : t == OpcodeType::MOVW_S ? 2 : 4;
        e.mnemonic = Fmt("mov.%s r%u,@r%u", Sfx(sz), m, n);
        c = Wr(sz, Fmt("R[%u]", n), Fmt("R[%u]", m));
        break;
    }
    case OpcodeType::MOVB_S0:
    case OpcodeType::MOVW_S0:
    case OpcodeType::MOVL_S0: {
        const unsigned sz = t == OpcodeType::MOVB_S0 ? 1 : t == OpcodeType::MOVW_S0 ? 2 : 4;
        e.mnemonic = Fmt("mov.%s r%u,@(r0,r%u)", Sfx(sz), m, n);
        c = Wr(sz, Fmt("R[0] + R[%u]", n), Fmt("R[%u]", m));
        break;
    }
    case OpcodeType::MOVB_S4:
    case OpcodeType::MOVW_S4: {
        // nd4 form: the base register is in bits 4-7
        const unsigned sz = t == OpcodeType::MOVB_S4 ? 1 : 2;
        const unsigned d = D4(op) * sz;
        e.mnemonic = Fmt("mov.%s r0,@(%u,r%u)", Sfx(sz), d, m);
        c = Wr(sz, Fmt("R[%u] + %uu", m, d), "R[0]");
        break;
    }
    case OpcodeType::MOVL_S4: {
        const unsigned d = D4(op) * 4;
        e.mnemonic = Fmt("mov.l r%u,@(%u,r%u)", m, d, n);
        c = Wr(4, Fmt("R[%u] + %uu", n, d), Fmt("R[%u]", m));
        break;
    }
    case OpcodeType::MOVB_SG:
    case OpcodeType::MOVW_SG:
    case OpcodeType::MOVL_SG: {
        const unsigned sz = t == OpcodeType::MOVB_SG ? 1 : t == OpcodeType::MOVW_SG ? 2 : 4;
        const unsigned d = U8(op) * sz;
        e.mnemonic = Fmt("mov.%s r0,@(%u,gbr)", Sfx(sz), d);
        c = Wr(sz, Fmt("GBR + %uu", d), "R[0]");
        break;
    }
    case OpcodeType::MOV_I:
        e.mnemonic = Fmt("mov #%d,r%u", S8(op), n);
        c = Fmt("R[%u] = 0x%08Xu;", n, (uint32_t)S8(op));
        break;
    case OpcodeType::MOVW_I: {
        // Ymir: address = PC + disp + 4 (no alignment), read through the fetch path
        if (slotPC) {
            e.mnemonic = Fmt("mov.w @(%u,pc),r%u", U8(op) * 2, n);
            c = Fmt("R[%u] = (uint32_t)(int32_t)(int16_t)cpu.RecompReadCode16C(%s + %uu, cyc);", n, pcExpr.c_str(),
                    U8(op) * 2 + 4);
            break;
        }
        const uint32_t a = pc + U8(op) * 2 + 4;
        e.mnemonic = Fmt("mov.w @(0x%08X),r%u", a, n);
        c = Fmt("R[%u] = (uint32_t)(int32_t)(int16_t)cpu.RecompReadCode16C(0x%08Xu, cyc);", n, a);
        break;
    }
    case OpcodeType::MOVL_I: {
        if (slotPC) {
            e.mnemonic = Fmt("mov.l @(%u,pc),r%u", U8(op) * 4, n);
            c = Fmt("R[%u] = cpu.RecompReadCode32C((%s & ~3u) + %uu, cyc);", n, pcExpr.c_str(), U8(op) * 4 + 4);
            break;
        }
        const uint32_t a = (pc & ~3u) + U8(op) * 4 + 4;
        e.mnemonic = Fmt("mov.l @(0x%08X),r%u", a, n);
        c = Fmt("R[%u] = cpu.RecompReadCode32C(0x%08Xu, cyc);", n, a);
        break;
    }
    case OpcodeType::MOVA: {
        if (slotPC) {
            e.mnemonic = Fmt("mova @(%u,pc),r0", U8(op) * 4);
            c = Fmt("R[0] = (%s & ~3u) + %uu;", pcExpr.c_str(), U8(op) * 4 + 4);
            break;
        }
        const uint32_t a = (pc & ~3u) + U8(op) * 4 + 4;
        e.mnemonic = Fmt("mova 0x%08X,r0", a);
        c = Fmt("R[0] = 0x%08Xu;", a);
        break;
    }
    case OpcodeType::MOVT:
        e.mnemonic = Fmt("movt r%u", n);
        c = Fmt("R[%u] = SR.T;", n);
        break;
    case OpcodeType::CLRT: e.mnemonic = "clrt"; c = "SR.T = 0;"; break;
    case OpcodeType::SETT: e.mnemonic = "sett"; c = "SR.T = 1;"; break;
    case OpcodeType::EXTUB:
        e.mnemonic = Fmt("extu.b r%u,r%u", m, n);
        c = Fmt("R[%u] = R[%u] & 0xFFu;", n, m);
        break;
    case OpcodeType::EXTUW:
        e.mnemonic = Fmt("extu.w r%u,r%u", m, n);
        c = Fmt("R[%u] = R[%u] & 0xFFFFu;", n, m);
        break;
    case OpcodeType::EXTSB:
        e.mnemonic = Fmt("exts.b r%u,r%u", m, n);
        c = Fmt("R[%u] = (uint32_t)(int32_t)(int8_t)R[%u];", n, m);
        break;
    case OpcodeType::EXTSW:
        e.mnemonic = Fmt("exts.w r%u,r%u", m, n);
        c = Fmt("R[%u] = (uint32_t)(int32_t)(int16_t)R[%u];", n, m);
        break;
    case OpcodeType::SWAPB:
        e.mnemonic = Fmt("swap.b r%u,r%u", m, n);
        c = Fmt("{ const uint32_t v = R[%u]; R[%u] = (v & 0xFFFF0000u) | ((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu); }",
                m, n);
        break;
    case OpcodeType::SWAPW:
        e.mnemonic = Fmt("swap.w r%u,r%u", m, n);
        c = Fmt("{ const uint32_t v = R[%u]; R[%u] = (v << 16) | (v >> 16); }", m, n);
        break;
    case OpcodeType::XTRCT:
        e.mnemonic = Fmt("xtrct r%u,r%u", m, n);
        c = Fmt("R[%u] = (R[%u] >> 16) | (R[%u] << 16);", n, n, m);
        break;

    // ---- system registers ------------------------------------------------
    // LDC Rm,SR and LDC.L @Rm+,SR can unmask interrupts: left to the
    // interpreter, which re-evaluates pending interrupts.
    case OpcodeType::LDC_SR_R: return unsupported("ldc rm,sr");
    case OpcodeType::LDC_SR_M: return unsupported("ldc.l @rm+,sr");

    case OpcodeType::LDC_GBR_R:
        e.mnemonic = Fmt("ldc r%u,gbr", n);
        c = Fmt("GBR = R[%u];", n);
        e.inhibitsInterrupt = true;
        break;
    case OpcodeType::LDC_VBR_R:
        e.mnemonic = Fmt("ldc r%u,vbr", n);
        c = Fmt("VBR = R[%u];", n);
        e.inhibitsInterrupt = true;
        break;
    case OpcodeType::LDS_MACH_R:
        e.mnemonic = Fmt("lds r%u,mach", n);
        c = Fmt("MAC.H = R[%u];", n);
        e.inhibitsInterrupt = true;
        break;
    case OpcodeType::LDS_MACL_R:
        e.mnemonic = Fmt("lds r%u,macl", n);
        c = Fmt("MAC.L = R[%u];", n);
        e.inhibitsInterrupt = true;
        break;
    case OpcodeType::LDS_PR_R:
        e.mnemonic = Fmt("lds r%u,pr", n);
        c = Fmt("PR = R[%u];", n);
        e.inhibitsInterrupt = true;
        break;
    case OpcodeType::STC_GBR_R:
        e.mnemonic = Fmt("stc gbr,r%u", n);
        c = Fmt("R[%u] = GBR;", n);
        e.inhibitsInterrupt = true;
        break;
    case OpcodeType::STC_SR_R:
        e.mnemonic = Fmt("stc sr,r%u", n);
        c = Fmt("R[%u] = SR.u32;", n);
        e.inhibitsInterrupt = true;
        break;
    case OpcodeType::STC_VBR_R:
        e.mnemonic = Fmt("stc vbr,r%u", n);
        c = Fmt("R[%u] = VBR;", n);
        e.inhibitsInterrupt = true;
        break;
    case OpcodeType::STS_MACH_R:
        e.mnemonic = Fmt("sts mach,r%u", n);
        c = Fmt("R[%u] = MAC.H;", n);
        e.inhibitsInterrupt = true;
        break;
    case OpcodeType::STS_MACL_R:
        e.mnemonic = Fmt("sts macl,r%u", n);
        c = Fmt("R[%u] = MAC.L;", n);
        e.inhibitsInterrupt = true;
        break;
    case OpcodeType::STS_PR_R:
        e.mnemonic = Fmt("sts pr,r%u", n);
        c = Fmt("R[%u] = PR;", n);
        e.inhibitsInterrupt = true;
        break;

    case OpcodeType::LDC_GBR_M:
    case OpcodeType::LDC_VBR_M:
    case OpcodeType::LDS_MACH_M:
    case OpcodeType::LDS_MACL_M:
    case OpcodeType::LDS_PR_M: {
        const char *dst = t == OpcodeType::LDC_GBR_M   ? "GBR"
                          : t == OpcodeType::LDC_VBR_M ? "VBR"
                          : t == OpcodeType::LDS_MACH_M ? "MAC.H"
                          : t == OpcodeType::LDS_MACL_M ? "MAC.L"
                                                        : "PR";
        e.mnemonic = Fmt("ld.l @r%u+,%s", n, dst);
        c = Fmt("%s = cpu.RecompRead32C(R[%u], cyc); R[%u] += 4u;", dst, n, n);
        e.inhibitsInterrupt = true;
        e.cycles = 3;
        break;
    }
    case OpcodeType::STC_GBR_M:
    case OpcodeType::STC_SR_M:
    case OpcodeType::STC_VBR_M:
    case OpcodeType::STS_MACH_M:
    case OpcodeType::STS_MACL_M:
    case OpcodeType::STS_PR_M: {
        const char *src = t == OpcodeType::STC_GBR_M    ? "GBR"
                          : t == OpcodeType::STC_SR_M   ? "SR.u32"
                          : t == OpcodeType::STC_VBR_M  ? "VBR"
                          : t == OpcodeType::STS_MACH_M ? "MAC.H"
                          : t == OpcodeType::STS_MACL_M ? "MAC.L"
                                                        : "PR";
        e.mnemonic = Fmt("st.l %s,@-r%u", src, n);
        c = Fmt("R[%u] -= 4u; cpu.RecompWrite32C(R[%u], %s, cyc);", n, n, src);
        e.inhibitsInterrupt = true;
        e.cycles = 2;
        break;
    }

    // ---- arithmetic --------------------------------------------------------
    case OpcodeType::ADD:
        e.mnemonic = Fmt("add r%u,r%u", m, n);
        c = Fmt("R[%u] += R[%u];", n, m);
        break;
    case OpcodeType::ADD_I:
        e.mnemonic = Fmt("add #%d,r%u", S8(op), n);
        c = Fmt("R[%u] += 0x%08Xu;", n, (uint32_t)S8(op));
        break;
    case OpcodeType::ADDC:
        e.mnemonic = Fmt("addc r%u,r%u", m, n);
        c = Fmt("{ const uint32_t t0 = R[%u]; const uint32_t t1 = t0 + R[%u]; R[%u] = t1 + SR.T; "
                "SR.T = (t0 > t1) || (t1 > R[%u]); }",
                n, m, n, n);
        break;
    case OpcodeType::ADDV:
        e.mnemonic = Fmt("addv r%u,r%u", m, n);
        c = Fmt("{ const bool dst = (int32_t)R[%u] < 0; const bool src = (int32_t)R[%u] < 0; R[%u] += R[%u]; "
                "const bool ans = ((int32_t)R[%u] < 0) ^ dst; SR.T = (src == dst) & ans; }",
                n, m, n, m, n);
        break;
    case OpcodeType::SUB:
        e.mnemonic = Fmt("sub r%u,r%u", m, n);
        c = Fmt("R[%u] -= R[%u];", n, m);
        break;
    case OpcodeType::SUBC:
        e.mnemonic = Fmt("subc r%u,r%u", m, n);
        c = Fmt("{ const uint32_t t0 = R[%u]; const uint32_t t1 = t0 - R[%u]; R[%u] = t1 - SR.T; "
                "SR.T = (t0 < t1) || (t1 < R[%u]); }",
                n, m, n, n);
        break;
    case OpcodeType::SUBV:
        e.mnemonic = Fmt("subv r%u,r%u", m, n);
        c = Fmt("{ const bool dst = (int32_t)R[%u] < 0; const bool src = (int32_t)R[%u] < 0; R[%u] -= R[%u]; "
                "const bool ans = ((int32_t)R[%u] < 0) ^ dst; SR.T = (src != dst) & ans; }",
                n, m, n, m, n);
        break;
    case OpcodeType::NEG:
        e.mnemonic = Fmt("neg r%u,r%u", m, n);
        c = Fmt("R[%u] = 0u - R[%u];", n, m);
        break;
    case OpcodeType::NEGC:
        e.mnemonic = Fmt("negc r%u,r%u", m, n);
        c = Fmt("{ const uint32_t t = 0u - R[%u]; R[%u] = t - SR.T; SR.T = (0u < t) || (t < R[%u]); }", m, n, n);
        break;
    case OpcodeType::DT:
        e.mnemonic = Fmt("dt r%u", n);
        c = Fmt("SR.T = --R[%u] == 0;", n);
        break;

    case OpcodeType::CLRMAC: e.mnemonic = "clrmac"; c = "MAC.u64 = 0;"; break;
    case OpcodeType::MUL:
        e.mnemonic = Fmt("mul.l r%u,r%u", m, n);
        c = Fmt("MAC.L = R[%u] * R[%u];", m, n);
        e.cycles = 2;
        break;
    case OpcodeType::MULS:
        e.mnemonic = Fmt("muls.w r%u,r%u", m, n);
        c = Fmt("MAC.L = (uint32_t)((int32_t)(int16_t)R[%u] * (int32_t)(int16_t)R[%u]);", m, n);
        break;
    case OpcodeType::MULU:
        e.mnemonic = Fmt("mulu.w r%u,r%u", m, n);
        c = Fmt("MAC.L = (uint32_t)(uint16_t)R[%u] * (uint32_t)(uint16_t)R[%u];", m, n);
        break;
    case OpcodeType::DMULS:
        e.mnemonic = Fmt("dmuls.l r%u,r%u", m, n);
        c = Fmt("MAC.u64 = (uint64_t)((int64_t)(int32_t)R[%u] * (int64_t)(int32_t)R[%u]);", m, n);
        e.cycles = 3;
        break;
    case OpcodeType::DMULU:
        e.mnemonic = Fmt("dmulu.l r%u,r%u", m, n);
        c = Fmt("MAC.u64 = (uint64_t)R[%u] * (uint64_t)R[%u];", m, n);
        e.cycles = 3;
        break;
    case OpcodeType::MACW:
        e.mnemonic = Fmt("mac.w @r%u+,@r%u+", m, n);
        c = Fmt("{ const int32_t op2 = (int16_t)cpu.RecompRead16C(R[%u], cyc); R[%u] += 2u;\n"
                "      const int32_t op1 = (int16_t)cpu.RecompRead16C(R[%u], cyc); R[%u] += 2u;\n"
                "      const int32_t mul = op1 * op2;\n"
                "      if (SR.S) {\n"
                "          const int64_t res = (int64_t)(int32_t)MAC.L + mul;\n"
                "          const int64_t sat = res < -0x80000000LL ? -0x80000000LL : res > 0x7FFFFFFFLL ? 0x7FFFFFFFLL : res;\n"
                "          if (res == sat) { MAC.L = (uint32_t)res; } else { MAC.L = (uint32_t)sat; MAC.H |= 1; }\n"
                "      } else {\n"
                "          MAC.u64 += (uint64_t)(int64_t)mul;\n"
                "      } }",
                n, n, m, m);
        e.cycles = 3;
        break;
    case OpcodeType::MACL:
        e.mnemonic = Fmt("mac.l @r%u+,@r%u+", m, n);
        c = Fmt("{ const int64_t op2 = (int32_t)cpu.RecompRead32C(R[%u], cyc); R[%u] += 4u;\n"
                "      const int64_t op1 = (int32_t)cpu.RecompRead32C(R[%u], cyc); R[%u] += 4u;\n"
                "      const int64_t mul = op1 * op2;\n"
                "      int64_t res = (int64_t)((uint64_t)mul + MAC.u64);\n"
                "      // Ymir compares the signed sum against unsigned limits: unsigned compare.\n"
                "      if (SR.S && (uint64_t)res > 0x00007FFFFFFFFFFFull && (uint64_t)res < 0xFFFF800000000000ull) {\n"
                "          res = ((int32_t)(op1 ^ op2) < 0) ? (int64_t)0xFFFF800000000000ull : 0x00007FFFFFFFFFFFLL;\n"
                "      }\n"
                "      MAC.u64 = (uint64_t)res; }",
                n, n, m, m);
        e.cycles = 3;
        break;

    case OpcodeType::DIV0S:
        e.mnemonic = Fmt("div0s r%u,r%u", m, n);
        c = Fmt("SR.M = (int32_t)R[%u] < 0; SR.Q = (int32_t)R[%u] < 0; SR.T = SR.M != SR.Q;", m, n);
        break;
    case OpcodeType::DIV0U: e.mnemonic = "div0u"; c = "SR.M = 0; SR.Q = 0; SR.T = 0;"; break;
    case OpcodeType::DIV1:
        e.mnemonic = Fmt("div1 r%u,r%u", m, n);
        c = Fmt("{ const bool oldQ = SR.Q; const bool Mb = SR.M; bool Q = (int32_t)R[%u] < 0;\n"
                "      R[%u] = (R[%u] << 1) | SR.T;\n"
                "      const uint32_t prev = R[%u];\n"
                "      if (oldQ == Mb) R[%u] -= R[%u]; else R[%u] += R[%u];\n"
                "      if (oldQ) { if (Mb) Q ^= R[%u] <= prev; else Q ^= R[%u] < prev; }\n"
                "      else      { if (Mb) Q ^= R[%u] >= prev; else Q ^= R[%u] > prev; }\n"
                "      SR.T = Q == Mb; SR.Q = Q; }",
                n, n, n, n, n, m, n, m, n, n, n, n);
        break;

    // ---- compare / test --------------------------------------------------
    case OpcodeType::CMP_EQ_I:
        e.mnemonic = Fmt("cmp/eq #%d,r0", S8(op));
        c = Fmt("SR.T = R[0] == 0x%08Xu;", (uint32_t)S8(op));
        break;
    case OpcodeType::CMP_EQ_R:
        e.mnemonic = Fmt("cmp/eq r%u,r%u", m, n);
        c = Fmt("SR.T = R[%u] == R[%u];", n, m);
        break;
    case OpcodeType::CMP_GE:
        e.mnemonic = Fmt("cmp/ge r%u,r%u", m, n);
        c = Fmt("SR.T = (int32_t)R[%u] >= (int32_t)R[%u];", n, m);
        break;
    case OpcodeType::CMP_GT:
        e.mnemonic = Fmt("cmp/gt r%u,r%u", m, n);
        c = Fmt("SR.T = (int32_t)R[%u] > (int32_t)R[%u];", n, m);
        break;
    case OpcodeType::CMP_HI:
        e.mnemonic = Fmt("cmp/hi r%u,r%u", m, n);
        c = Fmt("SR.T = R[%u] > R[%u];", n, m);
        break;
    case OpcodeType::CMP_HS:
        e.mnemonic = Fmt("cmp/hs r%u,r%u", m, n);
        c = Fmt("SR.T = R[%u] >= R[%u];", n, m);
        break;
    case OpcodeType::CMP_PL:
        e.mnemonic = Fmt("cmp/pl r%u", n);
        c = Fmt("SR.T = (int32_t)R[%u] > 0;", n);
        break;
    case OpcodeType::CMP_PZ:
        e.mnemonic = Fmt("cmp/pz r%u", n);
        c = Fmt("SR.T = (int32_t)R[%u] >= 0;", n);
        break;
    case OpcodeType::CMP_STR:
        e.mnemonic = Fmt("cmp/str r%u,r%u", m, n);
        c = Fmt("{ const uint32_t t = R[%u] ^ R[%u]; SR.T = !(((t >> 24) & 0xFFu) && ((t >> 16) & 0xFFu) && "
                "((t >> 8) & 0xFFu) && (t & 0xFFu)); }",
                m, n);
        break;
    case OpcodeType::TST_R:
        e.mnemonic = Fmt("tst r%u,r%u", m, n);
        c = Fmt("SR.T = (R[%u] & R[%u]) == 0;", n, m);
        break;
    case OpcodeType::TST_I:
        e.mnemonic = Fmt("tst #0x%02X,r0", U8(op));
        c = Fmt("SR.T = (R[0] & 0x%02Xu) == 0;", U8(op));
        break;
    case OpcodeType::TST_M:
        e.mnemonic = Fmt("tst.b #0x%02X,@(r0,gbr)", U8(op));
        c = Fmt("SR.T = (cpu.RecompRead8C(GBR + R[0], cyc) & 0x%02Xu) == 0;", U8(op));
        e.cycles = 3;
        break;
    case OpcodeType::TAS:
        e.mnemonic = Fmt("tas.b @r%u", n);
        c = Fmt("{ cyc += cpu.RecompAccessCyclesRMWByte(R[%u]); const uint8_t v = cpu.RecompRead8(R[%u]); SR.T = v == 0; "
                "cpu.RecompWrite8(R[%u], v | 0x80); }",
                n, n, n);
        e.cycles = 4;
        break;

    // ---- logic -------------------------------------------------------------
    case OpcodeType::AND_R:
        e.mnemonic = Fmt("and r%u,r%u", m, n);
        c = Fmt("R[%u] &= R[%u];", n, m);
        break;
    case OpcodeType::AND_I:
        e.mnemonic = Fmt("and #0x%02X,r0", U8(op));
        c = Fmt("R[0] &= 0x%02Xu;", U8(op));
        break;
    case OpcodeType::OR_R:
        e.mnemonic = Fmt("or r%u,r%u", m, n);
        c = Fmt("R[%u] |= R[%u];", n, m);
        break;
    case OpcodeType::OR_I:
        e.mnemonic = Fmt("or #0x%02X,r0", U8(op));
        c = Fmt("R[0] |= 0x%02Xu;", U8(op));
        break;
    case OpcodeType::XOR_R:
        e.mnemonic = Fmt("xor r%u,r%u", m, n);
        c = Fmt("R[%u] ^= R[%u];", n, m);
        break;
    case OpcodeType::XOR_I:
        e.mnemonic = Fmt("xor #0x%02X,r0", U8(op));
        c = Fmt("R[0] ^= 0x%02Xu;", U8(op));
        break;
    case OpcodeType::AND_M:
    case OpcodeType::OR_M:
    case OpcodeType::XOR_M: {
        const char *o = t == OpcodeType::AND_M ? "&" : t == OpcodeType::OR_M ? "|" : "^";
        e.mnemonic = Fmt("%s.b #0x%02X,@(r0,gbr)", t == OpcodeType::AND_M ? "and" : t == OpcodeType::OR_M ? "or" : "xor",
                         U8(op));
        // Ymir charges the access cycles of byte, word and long accesses for
        // and.b, or.b and xor.b respectively; the accesses themselves are bytes.
        const unsigned csz = t == OpcodeType::AND_M ? 1 : t == OpcodeType::OR_M ? 2 : 4;
        c = Fmt("{ const uint32_t a = GBR + R[0]; cyc += cpu.RecompAccessCycles(a, %u, false) + "
                "cpu.RecompAccessCycles(a, %u, true); cpu.RecompWrite8(a, (uint8_t)(cpu.RecompRead8(a) %s 0x%02Xu)); }",
                csz, csz, o, U8(op));
        e.cycles = 3;
        break;
    }
    case OpcodeType::NOT:
        e.mnemonic = Fmt("not r%u,r%u", m, n);
        c = Fmt("R[%u] = ~R[%u];", n, m);
        break;

    // ---- shifts / rotates ----------------------------------------------------
    case OpcodeType::ROTL:
        e.mnemonic = Fmt("rotl r%u", n);
        c = Fmt("SR.T = R[%u] >> 31; R[%u] = (R[%u] << 1) | SR.T;", n, n, n);
        break;
    case OpcodeType::ROTR:
        e.mnemonic = Fmt("rotr r%u", n);
        c = Fmt("SR.T = R[%u] & 1u; R[%u] = (R[%u] >> 1) | ((uint32_t)SR.T << 31);", n, n, n);
        break;
    case OpcodeType::ROTCL:
        e.mnemonic = Fmt("rotcl r%u", n);
        c = Fmt("{ const bool t = R[%u] >> 31; R[%u] = (R[%u] << 1) | SR.T; SR.T = t; }", n, n, n);
        break;
    case OpcodeType::ROTCR:
        e.mnemonic = Fmt("rotcr r%u", n);
        c = Fmt("{ const bool t = R[%u] & 1u; R[%u] = (R[%u] >> 1) | ((uint32_t)SR.T << 31); SR.T = t; }", n, n, n);
        break;
    case OpcodeType::SHAL:
    case OpcodeType::SHLL:
        e.mnemonic = Fmt("%s r%u", t == OpcodeType::SHAL ? "shal" : "shll", n);
        c = Fmt("SR.T = R[%u] >> 31; R[%u] <<= 1;", n, n);
        break;
    case OpcodeType::SHAR:
        e.mnemonic = Fmt("shar r%u", n);
        c = Fmt("SR.T = R[%u] & 1u; R[%u] = (uint32_t)((int32_t)R[%u] >> 1);", n, n, n);
        break;
    case OpcodeType::SHLR:
        e.mnemonic = Fmt("shlr r%u", n);
        c = Fmt("SR.T = R[%u] & 1u; R[%u] >>= 1;", n, n);
        break;
    case OpcodeType::SHLL2: e.mnemonic = Fmt("shll2 r%u", n); c = Fmt("R[%u] <<= 2;", n); break;
    case OpcodeType::SHLL8: e.mnemonic = Fmt("shll8 r%u", n); c = Fmt("R[%u] <<= 8;", n); break;
    case OpcodeType::SHLL16: e.mnemonic = Fmt("shll16 r%u", n); c = Fmt("R[%u] <<= 16;", n); break;
    case OpcodeType::SHLR2: e.mnemonic = Fmt("shlr2 r%u", n); c = Fmt("R[%u] >>= 2;", n); break;
    case OpcodeType::SHLR8: e.mnemonic = Fmt("shlr8 r%u", n); c = Fmt("R[%u] >>= 8;", n); break;
    case OpcodeType::SHLR16: e.mnemonic = Fmt("shlr16 r%u", n); c = Fmt("R[%u] >>= 16;", n); break;

    // ---- control flow and exceptions: not straight-line ------------------
    case OpcodeType::BF: return unsupported("bf");
    case OpcodeType::BFS: return unsupported("bf/s");
    case OpcodeType::BT: return unsupported("bt");
    case OpcodeType::BTS: return unsupported("bt/s");
    case OpcodeType::BRA: return unsupported("bra");
    case OpcodeType::BRAF: return unsupported("braf");
    case OpcodeType::BSR: return unsupported("bsr");
    case OpcodeType::BSRF: return unsupported("bsrf");
    case OpcodeType::JMP: return unsupported("jmp");
    case OpcodeType::JSR: return unsupported("jsr");
    case OpcodeType::RTS: return unsupported("rts");
    case OpcodeType::RTE: return unsupported("rte");
    case OpcodeType::TRAPA: return unsupported("trapa");
    case OpcodeType::SLEEP: return unsupported("sleep");
    default: return unsupported("illegal");
    }
    ApplyCycleModel(e, t, op);
    return e;
}

std::string FilePrologue() {
    return "// Generated by sh2gen. Do not edit.\n"
           "#include <ymir_recomp/abi.h>\n\n"
           "using drydock::CPU;\n\n";
}


} // namespace sh2gen

// =============================================================================
// Functions with control flow

namespace sh2gen {

namespace {

    enum class Kind { Plain, CondBranch, CondDelayed, Bra, Bsr, Bsrf, Jsr, Braf, Jmp, Rts, Stop };

    struct Decoded {
        Kind kind = Kind::Plain;
        bool takenIfT = true; // bt/bt/s vs bf/bf/s
        uint32_t target = 0;  // static target, when known
        unsigned reg = 0;     // register operand of computed transfers
    };

    Decoded Classify(uint16_t op, uint32_t pc) {
        Decoded d;
        const OpcodeType t = DecodeTable::s_instance.opcodes[0][op];
        const int32_t d8 = (int8_t)(op & 0xFF);
        int32_t d12 = op & 0xFFF;
        if (d12 & 0x800) d12 -= 0x1000;
        switch (t) {
        case OpcodeType::BT: d.kind = Kind::CondBranch; d.takenIfT = true; d.target = pc + 4 + d8 * 2; break;
        case OpcodeType::BF: d.kind = Kind::CondBranch; d.takenIfT = false; d.target = pc + 4 + d8 * 2; break;
        case OpcodeType::BTS: d.kind = Kind::CondDelayed; d.takenIfT = true; d.target = pc + 4 + d8 * 2; break;
        case OpcodeType::BFS: d.kind = Kind::CondDelayed; d.takenIfT = false; d.target = pc + 4 + d8 * 2; break;
        case OpcodeType::BRA: d.kind = Kind::Bra; d.target = pc + 4 + d12 * 2; break;
        case OpcodeType::BSR: d.kind = Kind::Bsr; d.target = pc + 4 + d12 * 2; break;
        case OpcodeType::BSRF: d.kind = Kind::Bsrf; d.reg = N(op); break;
        case OpcodeType::JSR: d.kind = Kind::Jsr; d.reg = N(op); break;
        case OpcodeType::BRAF: d.kind = Kind::Braf; d.reg = N(op); break;
        case OpcodeType::JMP: d.kind = Kind::Jmp; d.reg = N(op); break;
        case OpcodeType::RTS: d.kind = Kind::Rts; break;
        default:
            d.kind = EmitInstruction(op, pc).status == EmitStatus::Ok ? Kind::Plain : Kind::Stop;
            break;
        }
        return d;
    }

    bool HasDelaySlot(Kind k) {
        return k != Kind::Plain && k != Kind::CondBranch && k != Kind::Stop;
    }

    // A delayed branch can only be compiled if its slot is a legal slot
    // instruction that the emitter handles.
    bool SlotOk(uint16_t slot, uint32_t slotPC) {
        if (DecodeTable::s_instance.opcodes[1][slot] == OpcodeType::IllegalSlot) return false;
        return EmitInstruction(slot, slotPC, "0").status == EmitStatus::Ok;
    }

} // namespace

FunctionInfo EmitFunction(const std::string &name, uint32_t entry, const CodeReader &read, std::string &out,
                          const std::vector<uint32_t> &extraEntries) {
    static constexpr uint32_t kMaxInstructions = 8192;

    FunctionInfo info;
    info.name = name;
    info.entry = entry;

    // ---- 1. walk direct control flow ---------------------------------------
    // `visited`: instructions emitted in linear order (delay slots of
    // compiled branches are emitted inline and are not in this set).
    // `labels`: places control can arrive at other than from pc - 2.
    std::map<uint32_t, uint16_t> visited;
    std::set<uint32_t> labels{entry};
    std::vector<uint32_t> work{entry};
    for (uint32_t e : extraEntries) {
        labels.insert(e);
        work.push_back(e);
    }
    std::set<uint32_t> callTargets;

    while (!work.empty() && visited.size() < kMaxInstructions) {
        uint32_t pc = work.back();
        work.pop_back();
        while (visited.size() < kMaxInstructions) {
            if (visited.count(pc)) break;
            uint16_t op;
            if (!read(pc, op)) break;
            visited[pc] = op;
            const Decoded d = Classify(op, pc);
            if (d.kind == Kind::Plain) {
                pc += 2;
                continue;
            }
            if (d.kind == Kind::Stop) {
                // The interpreter runs this one instruction (ldc SR, trapa,
                // sleep, rte...); native code resumes right after it, so code
                // such as an idle loop following an `ldc Rm,SR` stays native.
                if (DecodeTable::s_instance.opcodes[0][op] != OpcodeType::Illegal &&
                    DecodeTable::s_instance.opcodes[0][op] != OpcodeType::RTE) {
                    labels.insert(pc + 2);
                    work.push_back(pc + 2);
                }
                break;
            }

            uint16_t slot = 0;
            if (HasDelaySlot(d.kind)) {
                if (!read(pc + 2, slot) || !SlotOk(slot, pc + 2)) {
                    visited[pc] = op; // emitted as an interpreter exit
                    break;
                }
            }
            switch (d.kind) {
            case Kind::CondBranch:
                labels.insert(d.target);
                work.push_back(d.target);
                labels.insert(pc + 2);
                work.push_back(pc + 2);
                break;
            case Kind::CondDelayed:
                labels.insert(d.target);
                work.push_back(d.target);
                labels.insert(pc + 4);
                work.push_back(pc + 4);
                break;
            case Kind::Bra:
                labels.insert(d.target);
                work.push_back(d.target);
                break;
            case Kind::Bsr:
            case Kind::Bsrf:
            case Kind::Jsr:
                if (d.kind == Kind::Bsr) callTargets.insert(d.target);
                // the callee returns to pc + 4 through the dispatcher
                labels.insert(pc + 4);
                work.push_back(pc + 4);
                break;
            default: break; // braf, jmp, rts: leave through the dispatcher
            }
            break;
        }
    }
    // Every label must be an instruction this function emits.
    for (auto it = labels.begin(); it != labels.end();) {
        if (!visited.count(*it)) it = labels.erase(it);
        else ++it;
    }

    // ---- 2. emit -------------------------------------------------------------
    auto transfer = [&](uint32_t t) {
        if (labels.count(t)) return Fmt("goto L_%08X;", t);
        return Fmt("{ CPU::Exit(cpu, 0x%08Xu); return; }", t);
    };
    auto exitTo = [&](const std::string &expr) { return "CPU::Exit(cpu, " + expr + "); return;"; };

    // Load-use stalls, as Ymir charges them: `wb` is the pending load when
    // known statically (-1: unknown, read CPU::WB at run time). CPU::WB is
    // always kept up to date, so it is correct at every exit and label.
    constexpr int kUnknown = -1;
    int wb = kUnknown;
    // C++ expression for the stall of an instruction reading `regs`.
    auto stall = [&](const std::vector<uint8_t> &regs, bool twice) -> std::string {
        if (regs.empty()) return "";
        if (wb != kUnknown) {
            const bool hit = std::find(regs.begin(), regs.end(), (uint8_t)wb) != regs.end();
            return hit ? (twice ? " + 2" : " + 1") : "";
        }
        std::string cond;
        for (uint8_t r : regs) cond += (cond.empty() ? "" : " || ") + Fmt("WB == %uu", r);
        return Fmt(" + %s(%s)", twice ? "2 * " : "", cond.c_str());
    };
    auto setWB = [&](uint8_t v) -> std::string {
        if (wb == v) return "";
        wb = v;
        return Fmt(" WB = 0x%02Xu;", v);
    };
    // Cycle and pending-load bookkeeping for one emitted instruction.
    auto account = [&](const Emitted &e) {
        std::string r = Fmt("cyc += %u%s;", e.cycles, stall(e.wbRead, e.wbTwice).c_str());
        return r + setWB(e.wbSet);
    };

    out += "void " + name + "(drydock::Cpu &cpu) {\n";
    out += "    auto &R = CPU::R(cpu);\n"
           "    auto &SR = CPU::SR(cpu);\n"
           "    auto &MAC = CPU::MAC(cpu);\n"
           "    auto &GBR = CPU::GBR(cpu);\n"
           "    auto &VBR = CPU::VBR(cpu);\n"
           "    auto &PR = CPU::PR(cpu);\n"
           "    auto &cyc = CPU::Cycles(cpu);\n"
           "    auto &WB = CPU::WB(cpu);\n"
           "    (void)R; (void)SR; (void)MAC; (void)GBR; (void)VBR; (void)PR; (void)cyc; (void)WB;\n";
    out += "    switch (CPU::PC(cpu)) {\n";
    for (uint32_t l : labels) out += Fmt("    case 0x%08Xu: goto L_%08X;\n", l, l);
    out += "    default: return;\n    }\n";

    uint32_t expectNext = 0;
    bool fallsThrough = false;
    for (auto it = visited.begin(); it != visited.end(); ++it) {
        const uint32_t pc = it->first;
        const uint16_t op = it->second;
        if (fallsThrough && pc != expectNext) {
            // the previous instruction falls into code not emitted here
            out += "    " + transfer(expectNext) + "\n";
        }
        if (!fallsThrough || labels.count(pc)) wb = kUnknown;
        if (labels.count(pc)) {
            out += Fmt("L_%08X:\n", pc);
            out += Fmt("    if (CPU::ShouldYield(cpu)) { %s }\n", exitTo(Fmt("0x%08Xu", pc)).c_str());
        }
        info.instructions++;

        const Decoded d = Classify(op, pc);
        uint16_t slot = 0;
        const bool slotOk = HasDelaySlot(d.kind) && read(pc + 2, slot) && SlotOk(slot, pc + 2);

        if (d.kind == Kind::Plain) {
            const Emitted e = EmitInstruction(op, pc);
            out += Fmt("    // %08X  %04X  %s\n", pc, op, e.mnemonic.c_str());
            if (!e.code.empty()) out += "    " + e.code + "\n";
            out += "    " + account(e) + "\n";
            fallsThrough = true;
            expectNext = pc + 2;
            if (!visited.count(pc + 2)) {
                // ran off the end of readable code
                out += "    " + exitTo(Fmt("0x%08Xu", pc + 2)) + "\n";
                fallsThrough = false;
            }
            continue;
        }

        if (d.kind == Kind::Stop || (HasDelaySlot(d.kind) && !slotOk)) {
            out += Fmt("    // %08X  %04X  -> interpreter\n", pc, op);
            out += "    " + exitTo(Fmt("0x%08Xu", pc)) + "\n";
            info.interpreterExits++;
            fallsThrough = false;
            continue;
        }

        // Emits the delay slot for a taken branch whose target is `tgt` (a C++
        // expression); Ymir evaluates PC-relative slot operands at tgt - 2.
        // Every branch leaves no pending load before its slot runs.
        auto emitSlot = [&](const std::string &tgt) {
            const std::string slotPC = "(" + tgt + ") - 2u";
            const Emitted s = EmitInstruction(slot, pc + 2, slotPC.c_str());
            info.instructions++;
            std::string r = Fmt("        // %08X  %04X  %s  (delay slot)\n", pc + 2, slot, s.mnemonic.c_str());
            if (!s.code.empty()) r += "        " + s.code + "\n";
            wb = 0xFF;
            r += "        " + account(s) + "\n";
            return r;
        };
        // A delayed branch: its own cost (2 plus the stall on `regs`), then no pending load.
        auto branchCost = [&](const std::vector<uint8_t> &regs) {
            const std::string st = stall(regs, false);
            return Fmt("        cyc += 2%s;%s\n", st.c_str(), setWB(0xFF).c_str());
        };

        fallsThrough = false;
        switch (d.kind) {
        case Kind::CondBranch:
            out += Fmt("    // %08X  %04X  b%s 0x%08X\n", pc, op, d.takenIfT ? "t" : "f", d.target);
            if (const std::string w = setWB(0xFF); !w.empty()) out += "   " + w + "\n";
            out += Fmt("    if (%sSR.T) { cyc += 3; %s }\n", d.takenIfT ? "" : "!", transfer(d.target).c_str());
            out += "    cyc += 1;\n";
            fallsThrough = true;
            expectNext = pc + 2;
            break;
        case Kind::CondDelayed: {
            out += Fmt("    // %08X  %04X  b%s/s 0x%08X\n", pc, op, d.takenIfT ? "t" : "f", d.target);
            if (const std::string w = setWB(0xFF); !w.empty()) out += "   " + w + "\n";
            out += Fmt("    if (%sSR.T) {\n        cyc += 2;\n", d.takenIfT ? "" : "!");
            out += emitSlot(Fmt("0x%08Xu", d.target));
            out += "        " + transfer(d.target) + "\n    } else {\n        cyc += 1;\n";
            // not taken: the slot runs as an ordinary instruction
            const Emitted s = EmitInstruction(slot, pc + 2);
            out += Fmt("        // %08X  %04X  %s\n", pc + 2, slot, s.mnemonic.c_str());
            if (!s.code.empty()) out += "        " + s.code + "\n";
            wb = 0xFF;
            out += "        " + account(s) + "\n";
            out += "        " + transfer(pc + 4) + "\n    }\n";
            break;
        }
        case Kind::Bra:
            out += Fmt("    // %08X  %04X  bra 0x%08X\n    {\n", pc, op, d.target);
            out += branchCost({});
            out += emitSlot(Fmt("0x%08Xu", d.target));
            out += "        " + transfer(d.target) + "\n    }\n";
            break;
        case Kind::Bsr:
            out += Fmt("    // %08X  %04X  bsr 0x%08X\n    {\n        PR = 0x%08Xu;\n", pc, op, d.target, pc + 4);
            out += branchCost({0x10});
            out += emitSlot(Fmt("0x%08Xu", d.target));
            out += "        " + exitTo(Fmt("0x%08Xu", d.target)) + "\n    }\n";
            break;
        case Kind::Bsrf:
        case Kind::Jsr:
        case Kind::Braf:
        case Kind::Jmp: {
            const bool rel = d.kind == Kind::Bsrf || d.kind == Kind::Braf;
            const bool call = d.kind == Kind::Bsrf || d.kind == Kind::Jsr;
            const char *mn = d.kind == Kind::Bsrf ? "bsrf" : d.kind == Kind::Jsr ? "jsr" : d.kind == Kind::Braf ? "braf" : "jmp";
            out += Fmt("    // %08X  %04X  %s r%u\n    {\n", pc, op, mn, d.reg);
            // the target is sampled before the delay slot runs
            out += rel ? Fmt("        const uint32_t tgt = 0x%08Xu + R[%u];\n", pc + 4, d.reg)
                       : Fmt("        const uint32_t tgt = R[%u];\n", d.reg);
            if (call) out += Fmt("        PR = 0x%08Xu;\n", pc + 4);
            out += call ? branchCost({(uint8_t)d.reg, 0x10}) : branchCost({(uint8_t)d.reg});
            out += emitSlot("tgt");
            out += "        " + exitTo("tgt") + "\n    }\n";
            break;
        }
        case Kind::Rts:
            out += Fmt("    // %08X  %04X  rts\n    {\n        const uint32_t tgt = PR;\n", pc, op);
            out += branchCost({0x10});
            out += emitSlot("tgt");
            out += "        " + exitTo("tgt") + "\n    }\n";
            break;
        default: break;
        }
        if (d.kind == Kind::CondBranch && !visited.count(pc + 2)) {
            out += "    " + exitTo(Fmt("0x%08Xu", pc + 2)) + "\n";
            fallsThrough = false;
        }
    }
    if (fallsThrough) out += "    " + transfer(expectNext) + "\n";
    out += "}\n";

    info.labels.assign(labels.begin(), labels.end());
    info.callTargets.assign(callTargets.begin(), callTargets.end());
    return info;
}

} // namespace sh2gen
