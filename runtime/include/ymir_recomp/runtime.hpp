#pragma once

// Runtime that plugs recompiled code into Ymir.
//
//   ymir_recomp::Runtime rt(saturn);
//   rt.LoadGameLibrary("recomp/<product>.dll", error);  // or RegisterStatic()
//   rt.Attach();               // both SH-2s now run native code when possible
//
// Before every interpreted instruction the SH-2 asks the runtime whether it has
// native code for PC. The lookup is a flat array over High WRAM (where Saturn
// games run), so the common "no native code here" answer costs one load.
// Before entering, the 32-bit word at PC is compared with the one the code was
// compiled from; a mismatch (an overlay or self-modifying code replaced it)
// falls back to the interpreter.

#include "cpu.hpp"
#include "dispatch.hpp"

#include <ymir/sys/saturn.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace ymir_recomp {

class Runtime {
public:
    explicit Runtime(ymir::Saturn &saturn);
    ~Runtime();

    /// Declares a code module (the main executable or an overlay file) loaded
    /// at `base`. `checks` are (address, big-endian word) pairs of code inside
    /// it: native code of the module only runs while all of them are in
    /// memory, so overlays loaded at the same address never run each other's
    /// code. Returns the module index for Add().
    uint16_t AddModule(const char *name, uint32_t base, uint32_t size, const drydock::Check *checks,
                       uint32_t numChecks);

    /// Registration interface handed to generated code (abi.h).
    drydock::Registrar MakeRegistrar();

    /// Registers a game linked into the executable (its drydock_register).
    void RegisterStatic(void (*registerFn)(drydock::Registrar *));

    /// Loads a game built as a shared library (DLL / .so) and registers it,
    /// replacing whatever was registered before. On failure returns false and
    /// describes the problem in `error`; the runtime is left empty.
    bool LoadGameLibrary(const std::string &path, std::string &error);

    /// Forgets every registered module and entry (and unloads the library).
    void Clear();

    /// Where a game's library is expected: <dir>/recomp/<product>.dll (.so).
    static std::string GameLibraryPath(const std::string &dir, const std::string &product);

    /// Product number of the registered game (empty if none).
    const std::string &Product() const { return m_product; }

    /// Registers native code entered at `pc`; `signature` is the big-endian
    /// 32-bit word at (pc & ~3) the code was compiled from. Several modules may
    /// register the same address (overlays); the one whose code is loaded runs.
    void Add(uint32_t pc, drydock::NativeFn fn, uint32_t signature, uint16_t module = 0);

    /// Only use native code while a disc with this product number is loaded
    /// (empty: always). Code compiled for one game must never run for another
    /// that happens to have the same word at an entry address.
    void SetProduct(std::string product) { m_product = std::move(product); }

    /// Starts/stops using native code on both SH-2s. Setting the environment
    /// variable YMIR_RECOMP_OFF=1 keeps everything interpreted (for comparisons).
    void Attach();
    void Detach();

    /// Trace mode: run interpreted only and record every non-sequential
    /// control transfer target (call, jump, branch, return, exception).
    void StartTrace();
    void WriteTrace(FILE *out) const;

    struct Stats {
        uint64_t nativeEntries = 0;  // native functions entered with progress
        uint64_t noProgress = 0;     // entered but yielded immediately
        uint64_t signatureMismatch = 0;
        uint64_t nativeCycles = 0;            // cycles accounted by native code
        uint64_t interpretedInstructions = 0; // instructions left to the interpreter
    };
    const Stats &GetStats() const { return m_stats; }

    /// Prints the addresses whose code most often differed from the compiled
    /// code (only collected when YMIR_RECOMP_DEBUG is set).
    void WriteMismatchReport(FILE *out) const;
    size_t EntryCount() const { return m_count; }

    /// Opens a log file for errors: which disc was detected, whether native
    /// code is active and, on a crash, which guest function each SH-2 was
    /// running natively. A handful of lines per session.
    void OpenLog(const std::string &path);
    /// Log next to the executable (Windows) or in the current directory.
    void OpenDefaultLog();

    /// Share of SH-2 work done by native code over the last second, 0..1, or a
    /// negative value while native code is inactive. Updated by the emulator
    /// thread about once per second; safe to read from any thread (GUI OSD).
    float NativeShare() const { return m_nativeShare.load(std::memory_order_relaxed); }

    /// The attached runtime, if any (for the frontend's on-screen display).
    static Runtime *Active() { return s_active; }

private:
    static bool Dispatch(ymir::sh2::SH2 &cpu, uint64_t budget, void *ctx);
    static bool TraceHook(ymir::sh2::SH2 &cpu, uint64_t budget, void *ctx);

    struct Entry {
        drydock::NativeFn fn = nullptr;
        uint32_t signature = 0;
        uint16_t module = 0;
        int32_t next = -1; // next entry for the same address in m_more
    };
    struct Module {
        std::string name;
        uint32_t base = 0, size = 0;
        std::vector<std::pair<uint32_t, uint32_t>> checks;
    };
    bool ModuleLoaded(uint16_t module) const;

    static constexpr uint32_t kBase = 0x06000000u;
    static constexpr uint32_t kSize = 0x00100000u; // High WRAM, cached view

    uint32_t WordAt(uint32_t pc) const;
    void RefreshEnabled();
    void Log(const char *fmt, ...);
    void UpdateShare();
    void InstallCrashHandler();
    void RemoveCrashHandler();
    friend struct CrashHandlerAccess;

    ymir::Saturn &m_saturn;
    drydock::Cpu m_ctx[2]; // ABI view of the master and slave SH-2
    void *m_library = nullptr;
    std::vector<Entry> m_entries; // one per halfword of High WRAM
    std::vector<Entry> m_more;    // further entries for addresses shared by overlays
    std::vector<Module> m_modules{Module{"(default)", 0, 0, {}}};
    size_t m_count = 0;
    Stats m_stats;
    std::string m_product;
    bool m_enabled = false;
    bool m_forcedOff = false;
    uint32_t m_refreshCounter = 0;

    // logging
    FILE *m_log = nullptr;
    std::string m_lastDisc = "?";
    std::chrono::steady_clock::time_point m_start, m_lastShare;
    Stats m_statsAtLastShare;
    std::atomic<float> m_nativeShare{-1.0f};
    static inline Runtime *s_active = nullptr;
    uint32_t m_nativePC[2] = {0, 0};  // guest function each SH-2 is running natively (0: none)
    void *m_crashHandler = nullptr;

    // trace mode
    struct TraceCPU {
        uint32_t lastPC = 0;
        bool valid = false;
    } m_trace[2];
    std::vector<uint8_t> m_traceKinds; // per halfword: bitmask of transfer kinds
    bool m_debug = false;
    std::vector<uint64_t> m_mismatchCount; // per halfword, debug only
    std::vector<uint64_t> m_missCount;     // per halfword: interpreted without native code, debug only
    std::vector<uint64_t> m_outsideCount;  // per 1 MB region outside High WRAM, debug only
};

} // namespace ymir_recomp
