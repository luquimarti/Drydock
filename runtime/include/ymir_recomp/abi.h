#pragma once

// Drydock native code ABI.
//
// Recompiled code (sh2recomp output) is written against this header only, so
// it can be built separately from the emulator -- on the player's machine, as
// a shared library, with a bare compiler and no system headers or C runtime --
// and loaded by the runtime at run time. It can also be linked into the
// emulator directly; the interface is the same.
//
// The runtime fills one drydock::Cpu per SH-2 with pointers into Ymir's SH-2
// state and with accessors for memory (the same paths and cycle costs as the
// interpreter). Registration goes through drydock::Registrar.
//
// Build the library with -DDRYDOCK_FREESTANDING (no <cstdint>) and
// -DDRYDOCK_BUILD_LIBRARY (exports drydock_register / drydock_abi_version).

#define DRYDOCK_ABI_VERSION 1

#ifdef DRYDOCK_FREESTANDING
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef signed char int8_t;
typedef short int16_t;
typedef int int32_t;
typedef long long int64_t;
#else
    #include <cstdint>
#endif

#if defined(DRYDOCK_BUILD_LIBRARY)
    #if defined(_WIN32)
        #define DRYDOCK_EXPORT extern "C" __declspec(dllexport)
    #else
        #define DRYDOCK_EXPORT extern "C" __attribute__((visibility("default")))
    #endif
#else
    #define DRYDOCK_EXPORT extern "C"
#endif

namespace drydock {

// Same layouts as ymir::sh2::RegSR / RegMAC (checked by the runtime).
union SR {
    uint32_t u32;
    struct {
        uint32_t T : 1;
        uint32_t S : 1;
        uint32_t : 2;
        uint32_t ILevel : 4;
        uint32_t Q : 1;
        uint32_t M : 1;
    };
};

union MAC {
    uint64_t u64;
    struct {
        uint32_t L;
        uint32_t H;
    };
};

// One SH-2, as seen by recompiled code.
struct Cpu {
    // registers and execution state (pointers into the emulator's SH-2)
    uint32_t *R; // R0-R15
    uint32_t *PC;
    uint32_t *PR;
    uint32_t *GBR;
    uint32_t *VBR;
    SR *sr;
    MAC *mac;
    uint64_t *cycles;           // cycles executed in the current time slice
    uint8_t *wb;                // pending load (load-use stall), 0xFF = none, 0x10 = PR
    const bool *intrPending;    // an interrupt is waiting to be serviced
    uint64_t budget;            // yield once cycles reach this

    // memory, through the interpreter's own paths; the *C variants also add
    // the access cycles the interpreter charges
    void *sh2;
    uint8_t (*read8c)(void *sh2, uint32_t address, uint64_t *cycles);
    uint16_t (*read16c)(void *sh2, uint32_t address, uint64_t *cycles);
    uint32_t (*read32c)(void *sh2, uint32_t address, uint64_t *cycles);
    uint16_t (*readCode16c)(void *sh2, uint32_t address, uint64_t *cycles);
    uint32_t (*readCode32c)(void *sh2, uint32_t address, uint64_t *cycles);
    void (*write8c)(void *sh2, uint32_t address, uint8_t value, uint64_t *cycles);
    void (*write16c)(void *sh2, uint32_t address, uint16_t value, uint64_t *cycles);
    void (*write32c)(void *sh2, uint32_t address, uint32_t value, uint64_t *cycles);
    uint8_t (*read8)(void *sh2, uint32_t address);
    void (*write8)(void *sh2, uint32_t address, uint8_t value);
    uint64_t (*accessCycles)(void *sh2, uint32_t address, uint32_t size, bool write);
    uint64_t (*accessCyclesRMWByte)(void *sh2, uint32_t address);
    void (*refillPipeline)(void *sh2);

    // names used by generated code
    uint8_t RecompRead8C(uint32_t a, uint64_t &c) { return read8c(sh2, a, &c); }
    uint16_t RecompRead16C(uint32_t a, uint64_t &c) { return read16c(sh2, a, &c); }
    uint32_t RecompRead32C(uint32_t a, uint64_t &c) { return read32c(sh2, a, &c); }
    uint16_t RecompReadCode16C(uint32_t a, uint64_t &c) { return readCode16c(sh2, a, &c); }
    uint32_t RecompReadCode32C(uint32_t a, uint64_t &c) { return readCode32c(sh2, a, &c); }
    void RecompWrite8C(uint32_t a, uint8_t v, uint64_t &c) { write8c(sh2, a, v, &c); }
    void RecompWrite16C(uint32_t a, uint16_t v, uint64_t &c) { write16c(sh2, a, v, &c); }
    void RecompWrite32C(uint32_t a, uint32_t v, uint64_t &c) { write32c(sh2, a, v, &c); }
    uint8_t RecompRead8(uint32_t a) { return read8(sh2, a); }
    void RecompWrite8(uint32_t a, uint8_t v) { write8(sh2, a, v); }
    uint64_t RecompAccessCycles(uint32_t a, uint32_t size, bool write) { return accessCycles(sh2, a, size, write); }
    uint64_t RecompAccessCyclesRMWByte(uint32_t a) { return accessCyclesRMWByte(sh2, a); }
};

// Accessors used by generated code.
struct CPU {
    static uint32_t *&R(Cpu &c) { return c.R; }
    static uint32_t &PC(Cpu &c) { return *c.PC; }
    static uint32_t &PR(Cpu &c) { return *c.PR; }
    static uint32_t &GBR(Cpu &c) { return *c.GBR; }
    static uint32_t &VBR(Cpu &c) { return *c.VBR; }
    static drydock::SR &SR(Cpu &c) { return *c.sr; }
    static drydock::MAC &MAC(Cpu &c) { return *c.mac; }
    static uint64_t &Cycles(Cpu &c) { return *c.cycles; }
    static uint8_t &WB(Cpu &c) { return *c.wb; }
    static bool ShouldYield(Cpu &c) { return *c.intrPending || *c.cycles >= c.budget; }
    // Leaves native code with PC = `pc`. The SH-2 fetches instruction pairs;
    // resuming on the second half of a pair needs the pipeline refilled.
    static void Exit(Cpu &c, uint32_t pc) {
        *c.PC = pc;
        if (pc & 2u) c.refillPipeline(c.sh2);
    }
};

using NativeFn = void (*)(Cpu &);

// Registration. Generated code describes each module with static tables and
// hands them to the runtime.
struct Check {
    uint32_t address, word; // code word expected at `address` while the module is loaded
};
struct Entry {
    uint32_t pc;        // entry address
    uint32_t signature; // code word at (pc & ~3) the function was compiled from
    NativeFn fn;
};
struct Registrar {
    void *ctx;
    void (*setProduct)(void *ctx, const char *productNumber);
    uint16_t (*addModule)(void *ctx, const char *name, uint32_t base, uint32_t size, const Check *checks,
                          uint32_t numChecks);
    void (*addEntries)(void *ctx, uint16_t module, const Entry *entries, uint32_t numEntries);
};

} // namespace drydock

// Exported by every generated game: registers all its modules.
//   int  drydock_abi_version(void)      -> DRYDOCK_ABI_VERSION
//   void drydock_register(drydock::Registrar *)
