#pragma once
// Walks Frostbite's global TypeInfo list (runtime reflection).
//
// Runtime TypeInfo:      +0x00 TypeInfoData*, +0x08 next, +0x10 u16 runtime id, +0x12 flags
// ClassInfo adds:        +0x18 super, +0x28 class-range word (first | last << 16)
// TypeInfoData (static): +0x00 name, +0x08 u16 flags, +0x0A u16 size, +0x10 module,
//                        +0x18 u8 alignment, +0x1A u16 fieldCount, +0x20 kind-specific
// Same layout on PS4 and PC (confirmed by both dumps).

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace mecmp {

struct TypeEntry {
    uintptr_t ti = 0;
    uintptr_t data = 0;
    std::string name;
    uint16_t flags = 0;
    uint16_t size = 0;
    uint32_t basicType() const { return (flags >> 4) & 0x1f; } // 2 ValueType, 3 Class, 4 Array, 8 Enum
};

class TypeList {
public:
    // Number of entries reachable from the list head (0 if the head is empty/unreadable).
    static size_t countFrom(uintptr_t listHeadAddr);

    void load(uintptr_t listHeadAddr);
    size_t size() const { return entries_.size(); }
    const TypeEntry* find(const std::string& name) const;
    const std::vector<TypeEntry>& entries() const { return entries_; }

private:
    std::vector<TypeEntry> entries_;
    std::unordered_map<std::string, size_t> byName_;
};

} // namespace mecmp
