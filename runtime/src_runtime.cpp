#include <ymir_recomp/runtime.hpp>

#include <ymir/hw/sh2/sh2_decode.hpp>

#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <ctime>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

#include <cstring>
#include <type_traits>

namespace ymir_recomp {

using ymir::sh2::DecodeTable;
using ymir::sh2::OpcodeType;

// ---- ABI context ----------------------------------------------------------------

static_assert(sizeof(drydock::SR) == sizeof(ymir::sh2::RegSR), "SR layout differs from Ymir");
static_assert(sizeof(drydock::MAC) == sizeof(ymir::sh2::RegMAC), "MAC layout differs from Ymir");

namespace {
    using ymir::sh2::SH2;
    SH2 &S(void *p) { return *static_cast<SH2 *>(p); }
    uint8_t Read8C(void *p, uint32_t a, uint64_t *c) { return S(p).RecompRead8C(a, *c); }
    uint16_t Read16C(void *p, uint32_t a, uint64_t *c) { return S(p).RecompRead16C(a, *c); }
    uint32_t Read32C(void *p, uint32_t a, uint64_t *c) { return S(p).RecompRead32C(a, *c); }
    uint16_t ReadCode16C(void *p, uint32_t a, uint64_t *c) { return S(p).RecompReadCode16C(a, *c); }
    uint32_t ReadCode32C(void *p, uint32_t a, uint64_t *c) { return S(p).RecompReadCode32C(a, *c); }
    void Write8C(void *p, uint32_t a, uint8_t v, uint64_t *c) { S(p).RecompWrite8C(a, v, *c); }
    void Write16C(void *p, uint32_t a, uint16_t v, uint64_t *c) { S(p).RecompWrite16C(a, v, *c); }
    void Write32C(void *p, uint32_t a, uint32_t v, uint64_t *c) { S(p).RecompWrite32C(a, v, *c); }
    uint8_t Read8(void *p, uint32_t a) { return S(p).RecompRead8(a); }
    void Write8(void *p, uint32_t a, uint8_t v) { S(p).RecompWrite8(a, v); }
    uint64_t AccessCycles(void *p, uint32_t a, uint32_t size, bool write) {
        return S(p).RecompAccessCycles(a, size, write);
    }
    uint64_t AccessCyclesRMWByte(void *p, uint32_t a) { return S(p).RecompAccessCyclesRMWByte(a); }
    void Refill(void *p) { S(p).GetProbe().RefillPipeline(); }
} // namespace

drydock::Cpu CPU::MakeContext(SH2 &c) {
    drydock::Cpu x{};
    x.R = c.R.data();
    x.PC = &c.PC;
    x.PR = &c.PR;
    x.GBR = &c.GBR;
    x.VBR = &c.VBR;
    x.sr = reinterpret_cast<drydock::SR *>(&c.SR);
    x.mac = reinterpret_cast<drydock::MAC *>(&c.MAC);
    x.cycles = &c.m_cyclesExecuted;
    x.wb = &c.m_wbReg;
    x.intrPending = &c.m_intrFlags.pending;
    x.budget = ~0ull;
    x.sh2 = &c;
    x.read8c = Read8C;
    x.read16c = Read16C;
    x.read32c = Read32C;
    x.readCode16c = ReadCode16C;
    x.readCode32c = ReadCode32C;
    x.write8c = Write8C;
    x.write16c = Write16C;
    x.write32c = Write32C;
    x.read8 = Read8;
    x.write8 = Write8;
    x.accessCycles = AccessCycles;
    x.accessCyclesRMWByte = AccessCyclesRMWByte;
    x.refillPipeline = Refill;
    return x;
}

Runtime::Runtime(ymir::Saturn &saturn)
    : m_saturn(saturn)
    , m_entries(kSize / 2) {
    m_ctx[0] = CPU::MakeContext(saturn.masterSH2);
    m_ctx[1] = CPU::MakeContext(saturn.slaveSH2);
    m_debug = getenv("YMIR_RECOMP_DEBUG") != nullptr;
    const char *off = getenv("YMIR_RECOMP_OFF");
    m_forcedOff = off != nullptr && off[0] != '\0' && off[0] != '0';
    if (m_debug) {
        m_mismatchCount.assign(kSize / 2, 0);
        m_missCount.assign(kSize / 2, 0);
        m_outsideCount.assign(512, 0);
    }
    m_start = m_lastShare = std::chrono::steady_clock::now();
}

Runtime::~Runtime() {
    Detach();
    Clear();
    if (m_log) fclose(m_log);
}

// ---- log ------------------------------------------------------------------------

void Runtime::OpenLog(const std::string &path) {
    if (m_log) fclose(m_log);
    m_log = fopen(path.c_str(), "w");
    if (!m_log) return;
    const std::time_t now = std::time(nullptr);
    char when[64];
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    Log("YmirRecomp %s", when);
    Log("native entry points: %zu, modules: %zu, game: %s", m_count, m_modules.size() - 1,
        m_product.empty() ? "(any)" : m_product.c_str());
    if (m_forcedOff) Log("YMIR_RECOMP_OFF is set: everything runs in the interpreter");
}

void Runtime::OpenDefaultLog() {
    std::string path = "ymir-recomp.log";
#ifdef _WIN32
    char exe[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::string dir(exe, n);
        const size_t slash = dir.find_last_of("\\/");
        if (slash != std::string::npos) path = dir.substr(0, slash + 1) + path;
    }
#endif
    OpenLog(path);
}

void Runtime::Log(const char *fmt, ...) {
    if (!m_log) return;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
    fprintf(m_log, "[%8.1fs] ", t);
    va_list args;
    va_start(args, fmt);
    vfprintf(m_log, fmt, args);
    va_end(args);
    fputc('\n', m_log);
    fflush(m_log);
}

void Runtime::UpdateShare() {
    const Stats &a = m_statsAtLastShare, &b = m_stats;
    const uint64_t nat = b.nativeCycles - a.nativeCycles;
    const uint64_t interp = b.interpretedInstructions - a.interpretedInstructions;
    // interpreted instructions are counted as ~1 cycle each: an approximation
    m_nativeShare.store(!m_enabled ? -1.0f : (nat + interp) ? (float)((double)nat / (double)(nat + interp)) : 0.0f,
                        std::memory_order_relaxed);
    m_statsAtLastShare = m_stats;
}

// ---- crash report -------------------------------------------------------------------

namespace {
    Runtime *g_crashRuntime = nullptr;
}

struct CrashHandlerAccess {
    static void Report(Runtime &rt, unsigned long code, const void *hostAddr) {
        rt.Log("CRASH: exception 0x%08lX at %p", code, hostAddr);
        const char *names[2] = {"master", "slave"};
        ymir::sh2::SH2 *cpus[2] = {&rt.m_saturn.masterSH2, &rt.m_saturn.slaveSH2};
        for (int i = 0; i < 2; i++) {
            if (rt.m_nativePC[i])
                rt.Log("  %s SH-2: PC=%08X, inside native function %08X", names[i], CPU::PC(*cpus[i]),
                       rt.m_nativePC[i]);
            else
                rt.Log("  %s SH-2: PC=%08X, interpreting", names[i], CPU::PC(*cpus[i]));
        }
    }
};

#ifdef _WIN32
static LONG CALLBACK RecompCrashHandler(EXCEPTION_POINTERS *info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        if (g_crashRuntime) {
            Runtime *rt = g_crashRuntime;
            g_crashRuntime = nullptr; // report once
            CrashHandlerAccess::Report(*rt, code, info->ExceptionRecord->ExceptionAddress);
        }
        break;
    default: break;
    }
    return EXCEPTION_CONTINUE_SEARCH; // let Ymir show its own error dialog
}
#endif

void Runtime::InstallCrashHandler() {
#ifdef _WIN32
    if (m_crashHandler || !m_log) return;
    g_crashRuntime = this;
    m_crashHandler = AddVectoredExceptionHandler(1, RecompCrashHandler);
#endif
}

void Runtime::RemoveCrashHandler() {
#ifdef _WIN32
    if (m_crashHandler) RemoveVectoredExceptionHandler(m_crashHandler);
    m_crashHandler = nullptr;
    if (g_crashRuntime == this) g_crashRuntime = nullptr;
#endif
}

uint16_t Runtime::AddModule(const char *name, uint32_t base, uint32_t size, const drydock::Check *checks,
                            uint32_t numChecks) {
    Module m;
    m.name = name;
    m.base = base;
    m.size = size;
    for (uint32_t i = 0; i < numChecks; i++)
        if (checks[i].address >= kBase && checks[i].address + 4 <= kBase + kSize)
            m.checks.push_back({checks[i].address, checks[i].word});
    m_modules.push_back(std::move(m));
    return (uint16_t)(m_modules.size() - 1);
}

drydock::Registrar Runtime::MakeRegistrar() {
    drydock::Registrar r{};
    r.ctx = this;
    r.setProduct = [](void *ctx, const char *product) { static_cast<Runtime *>(ctx)->SetProduct(product); };
    r.addModule = [](void *ctx, const char *name, uint32_t base, uint32_t size, const drydock::Check *checks,
                     uint32_t numChecks) {
        return static_cast<Runtime *>(ctx)->AddModule(name, base, size, checks, numChecks);
    };
    r.addEntries = [](void *ctx, uint16_t module, const drydock::Entry *entries, uint32_t numEntries) {
        Runtime &rt = *static_cast<Runtime *>(ctx);
        for (uint32_t i = 0; i < numEntries; i++) rt.Add(entries[i].pc, entries[i].fn, entries[i].signature, module);
    };
    return r;
}

void Runtime::RegisterStatic(void (*registerFn)(drydock::Registrar *)) {
    drydock::Registrar r = MakeRegistrar();
    registerFn(&r);
    Log("native entry points: %zu, modules: %zu, game: %s (linked in)", m_count, m_modules.size() - 1,
        m_product.empty() ? "(any)" : m_product.c_str());
}

void Runtime::Clear() {
    std::fill(m_entries.begin(), m_entries.end(), Entry{});
    m_more.clear();
    m_modules.resize(1);
    m_count = 0;
    m_product.clear();
    m_lastDisc = "?";
    if (m_library) {
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(m_library));
#else
        dlclose(m_library);
#endif
        m_library = nullptr;
    }
}

std::string Runtime::GameLibraryPath(const std::string &dir, const std::string &product) {
    std::string d = dir;
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
#ifdef _WIN32
    return d + "recomp/" + product + ".dll";
#else
    return d + "recomp/" + product + ".so";
#endif
}

bool Runtime::LoadGameLibrary(const std::string &path, std::string &error) {
    Clear();
#ifdef _WIN32
    HMODULE lib = LoadLibraryA(path.c_str());
    if (!lib) {
        error = "cannot load " + path + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    auto sym = [&](const char *name) { return reinterpret_cast<void *>(GetProcAddress(lib, name)); };
#else
    void *lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        error = "cannot load " + path + ": " + dlerror();
        return false;
    }
    auto sym = [&](const char *name) { return dlsym(lib, name); };
#endif
    m_library = lib;
    auto version = reinterpret_cast<int (*)()>(sym("drydock_abi_version"));
    auto reg = reinterpret_cast<void (*)(drydock::Registrar *)>(sym("drydock_register"));
    if (!version || !reg) {
        error = path + " is not a Drydock game library";
        Clear();
        return false;
    }
    if (version() != DRYDOCK_ABI_VERSION) {
        error = path + " was built for another version of the runtime (ABI " + std::to_string(version()) +
                ", expected " + std::to_string(DRYDOCK_ABI_VERSION) + ")";
        Clear();
        return false;
    }
    drydock::Registrar r = MakeRegistrar();
    reg(&r);
    Log("loaded %s: %zu native entry points, %zu modules, game %s", path.c_str(), m_count, m_modules.size() - 1,
        m_product.c_str());
    RefreshEnabled();
    return true;
}

void Runtime::Add(uint32_t pc, drydock::NativeFn fn, uint32_t signature, uint16_t module) {
    if (pc < kBase || pc >= kBase + kSize || (pc & 1)) return;
    Entry &e = m_entries[(pc - kBase) >> 1];
    m_count++;
    if (!e.fn) {
        e.fn = fn;
        e.signature = signature;
        e.module = module;
        return;
    }
    // address shared by several modules: chain the new entry after the first
    Entry more{fn, signature, module, e.next};
    m_more.push_back(more);
    e.next = (int32_t)(m_more.size() - 1);
}

bool Runtime::ModuleLoaded(uint16_t module) const {
    for (const auto &[addr, word] : m_modules[module].checks)
        if (WordAt(addr) != word) return false;
    return true;
}

void Runtime::RefreshEnabled() {
    const std::string &pn = m_saturn.GetCDInterface().GetDiscHeader().productNumber;
    m_enabled = !m_forcedOff && (m_product.empty() || pn.compare(0, m_product.size(), m_product) == 0);
    if (m_log && pn != m_lastDisc) {
        m_lastDisc = pn;
        Log("disc detected: \"%s\" -> native code %s", pn.c_str(),
            m_enabled ? "ACTIVE" : (m_forcedOff ? "disabled (YMIR_RECOMP_OFF)" : "inactive (other game)"));
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastShare >= std::chrono::seconds(1)) {
        m_lastShare = now;
        UpdateShare();
    }
}

void Runtime::Attach() {
    s_active = this;
    if (!m_log && getenv("YMIR_RECOMP_NOLOG") == nullptr) OpenDefaultLog();
    InstallCrashHandler();
    RefreshEnabled();
    m_saturn.masterSH2.SetRecompDispatch(&Runtime::Dispatch, this);
    m_saturn.slaveSH2.SetRecompDispatch(&Runtime::Dispatch, this);
}

void Runtime::Detach() {
    if (s_active == this) s_active = nullptr;
    RemoveCrashHandler();
    m_saturn.masterSH2.SetRecompDispatch(nullptr, nullptr);
    m_saturn.slaveSH2.SetRecompDispatch(nullptr, nullptr);
}

uint32_t Runtime::WordAt(uint32_t pc) const {
    const uint8_t *p = &m_saturn.mem.WRAMHigh[(pc & ~3u) & (kSize - 1)];
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

bool Runtime::Dispatch(ymir::sh2::SH2 &cpu, uint64_t budget, void *ctx) {
    Runtime &rt = *static_cast<Runtime *>(ctx);
    // Re-check which disc is loaded every 64K dispatches (a fraction of a frame).
    if ((++rt.m_refreshCounter & 0xFFFFu) == 0) rt.RefreshEnabled();
    if (!rt.m_enabled) return false;
    const uint32_t pc = CPU::PC(cpu);
    const uint32_t off = pc - kBase;
    if (off >= kSize) {
        rt.m_stats.interpretedInstructions++;
        if (rt.m_debug) rt.m_outsideCount[(pc >> 20) & 0x1FF]++;
        return false;
    }
    const Entry *ep = &rt.m_entries[off >> 1];
    if (!ep->fn) {
        rt.m_stats.interpretedInstructions++;
        if (rt.m_debug) rt.m_missCount[off >> 1]++;
        return false;
    }
    // first entry whose code word is in memory and whose module is loaded
    const uint32_t word = rt.WordAt(pc);
    while (ep->signature != word || !rt.ModuleLoaded(ep->module)) {
        if (ep->next < 0) {
            rt.m_stats.signatureMismatch++;
            rt.m_stats.interpretedInstructions++;
            if (rt.m_debug) rt.m_mismatchCount[off >> 1]++;
            return false;
        }
        ep = &rt.m_more[ep->next];
    }
    const Entry &e = *ep;
    const uint64_t before = CPU::Cycles(cpu);
    const int idx = &cpu == &rt.m_saturn.masterSH2 ? 0 : 1;
    drydock::Cpu &x = rt.m_ctx[idx];
    x.budget = budget;
    rt.m_nativePC[idx] = pc;
    e.fn(x);
    rt.m_nativePC[idx] = 0;
    // No progress only if nothing ran: a loop that yields back at its own
    // entry (budget used up, e.g. an idle loop) did run.
    if (CPU::Cycles(cpu) == before) {
        rt.m_stats.noProgress++;
        rt.m_stats.interpretedInstructions++;
        return false;
    }
    rt.m_stats.nativeCycles += CPU::Cycles(cpu) - before;
    rt.m_stats.nativeEntries++;
    return true;
}

void Runtime::WriteMismatchReport(FILE *out) const {
    if (!m_debug) return;
    {
        // interpreted instructions in High WRAM without native code, by 256-byte block
        std::vector<std::pair<uint64_t, uint32_t>> blocks;
        std::vector<uint64_t> per(kSize / 256, 0);
        for (size_t i = 0; i < m_missCount.size(); i++) per[i * 2 / 256] += m_missCount[i];
        for (size_t b = 0; b < per.size(); b++)
            if (per[b]) blocks.push_back({per[b], kBase + (uint32_t)b * 256});
        std::sort(blocks.rbegin(), blocks.rend());
        fprintf(out, "[recomp] interpreted instructions without native code, top 256-byte blocks:\n");
        for (size_t i = 0; i < blocks.size() && i < 20; i++)
            fprintf(out, "  %08X  %llu\n", blocks[i].second, (unsigned long long)blocks[i].first);
        std::vector<std::pair<uint64_t, uint32_t>> addrs;
        for (size_t i = 0; i < m_missCount.size(); i++)
            if (m_missCount[i]) addrs.push_back({m_missCount[i], kBase + (uint32_t)i * 2});
        std::sort(addrs.rbegin(), addrs.rend());
        fprintf(out, "[recomp] top interpreted addresses without native code:\n");
        for (size_t i = 0; i < addrs.size() && i < 12; i++)
            fprintf(out, "  %08X  %llu\n", addrs[i].second, (unsigned long long)addrs[i].first);
        fprintf(out, "[recomp] interpreted outside High WRAM, by 1 MB region:\n");
        for (size_t r = 0; r < m_outsideCount.size(); r++)
            if (m_outsideCount[r]) fprintf(out, "  %08X  %llu\n", (uint32_t)r << 20, (unsigned long long)m_outsideCount[r]);
    }
    std::vector<std::pair<uint64_t, uint32_t>> v;
    for (size_t i = 0; i < m_mismatchCount.size(); i++)
        if (m_mismatchCount[i]) v.push_back({m_mismatchCount[i], kBase + (uint32_t)i * 2});
    std::sort(v.rbegin(), v.rend());
    fprintf(out, "[recomp] %zu addresses with signature mismatches; top:\n", v.size());
    for (size_t i = 0; i < v.size() && i < 15; i++) {
        const uint32_t pc = v[i].second;
        fprintf(out, "  %08X  %llu times  memory=%08X compiled=%08X\n", pc, (unsigned long long)v[i].first,
                WordAt(pc), m_entries[(pc - kBase) >> 1].signature);
    }
}

// ---- trace mode ---------------------------------------------------------------

namespace {
    enum : uint8_t {
        kCall = 1,      // jsr / bsr / bsrf target
        kJump = 2,      // jmp / braf target (tail call, jump table)
        kBranch = 4,    // bt / bf / bra target
        kReturn = 8,    // rts / rte target
        kException = 16 // interrupt or exception handler
    };

    uint8_t KindOf(OpcodeType t) {
        switch (t) {
        case OpcodeType::JSR: case OpcodeType::BSR: case OpcodeType::BSRF: return kCall;
        case OpcodeType::JMP: case OpcodeType::BRAF: return kJump;
        case OpcodeType::BT: case OpcodeType::BF: case OpcodeType::BTS: case OpcodeType::BFS:
        case OpcodeType::BRA: return kBranch;
        case OpcodeType::RTS: case OpcodeType::RTE: return kReturn;
        case OpcodeType::TRAPA: return kException;
        default: return 0;
        }
    }
} // namespace

void Runtime::StartTrace() {
    m_traceKinds.assign(kSize / 2, 0);
    m_trace[0] = m_trace[1] = {};
    m_saturn.masterSH2.SetRecompDispatch(&Runtime::TraceHook, this);
    m_saturn.slaveSH2.SetRecompDispatch(&Runtime::TraceHook, this);
}

bool Runtime::TraceHook(ymir::sh2::SH2 &cpu, uint64_t, void *ctx) {
    Runtime &rt = *static_cast<Runtime *>(ctx);
    TraceCPU &tc = rt.m_trace[&cpu == &rt.m_saturn.masterSH2 ? 0 : 1];
    const uint32_t pc = CPU::PC(cpu);
    // The hook is not called for delay slots, so after a delayed branch at B
    // the next PC seen is the target: B + 2 (the slot) never shows up.
    if (tc.valid && pc != tc.lastPC + 2 && pc - kBase < kSize) {
        uint8_t kind = kException;
        const uint32_t prev = tc.lastPC;
        if (prev - kBase < kSize) {
            const uint16_t op = (uint16_t)(rt.WordAt(prev) >> ((prev & 2) ? 0 : 16));
            const uint8_t k = KindOf(DecodeTable::s_instance.opcodes[0][op]);
            if (k) kind = k;
        } else {
            kind = kException | kReturn; // came from outside High WRAM (BIOS, low WRAM)
        }
        rt.m_traceKinds[(pc - kBase) >> 1] |= kind;
    }
    tc.lastPC = pc;
    tc.valid = true;
    return false;
}

void Runtime::WriteTrace(FILE *out) const {
    fprintf(out, "# transfer targets in High WRAM: ADDRESS KINDS (1=call 2=jump 4=branch 8=return 16=exception)\n");
    for (size_t i = 0; i < m_traceKinds.size(); i++)
        if (m_traceKinds[i]) fprintf(out, "%08X %u\n", kBase + (uint32_t)i * 2, m_traceKinds[i]);
}

} // namespace ymir_recomp
