#pragma once

// Native code lookup: guest address -> recompiled entry point.

#include "abi.h"

#include <cstdint>
#include <unordered_map>

namespace ymir_recomp {

using NativeFn = drydock::NativeFn;

class Table {
public:
    // Addresses are matched exactly: code reached through the cache-through
    // mirror (0x2...) is a different entry, because instruction fetch behaves
    // differently there when SH-2 cache emulation is enabled.
    void Add(uint32_t pc, NativeFn fn) { m_map[pc] = fn; }

    NativeFn Find(uint32_t pc) const {
        const auto it = m_map.find(pc);
        return it == m_map.end() ? nullptr : it->second;
    }

    size_t Size() const { return m_map.size(); }

private:
    std::unordered_map<uint32_t, NativeFn> m_map;
};

} // namespace ymir_recomp
