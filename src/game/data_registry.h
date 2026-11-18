#pragma once
// Read-only access to the game's data registry: every loaded data container, found without scanning the heap (3.4).
// Layout and addresses: engine.h (Registry_*).

#include <cstdint>
#include <vector>

namespace mecmp::game::registry {

struct Object {
    uint64_t key = 0;
    uintptr_t object = 0; // DataContainer*; its first qword is the vtable
};

static_assert(sizeof(Object) == 16, "registry entries are 16 bytes");

inline uint16_t partitionOf(uint64_t key) { return static_cast<uint16_t>(key >> 40); }
inline uint16_t classIdOf(uint64_t key) { return static_cast<uint16_t>(key >> 16); }
inline uint16_t indexOf(uint64_t key) { return static_cast<uint16_t>(key); }

// A key belongs to a type when its class id is in the type's [classId, lastSubClassId] (sub-classes included). A zero first
// id means the range was not read (the image layout differs): nothing matches.
inline bool inClassRange(uint64_t key, uint16_t first, uint16_t last) {
    const uint16_t c = classIdOf(key);
    return first != 0 && c >= first && c <= last;
}

// The game's registry loader never registers data from partitions firstSkipped..firstSkipped+count-1 (FUN_143119500), so an
// object that is loaded but absent from the registry may simply live in one of them. Constants: engine.h
// Registry_SkippedPartition*.
inline bool partitionSkipped(uint16_t partition, uint16_t firstSkipped, uint16_t count) {
    return static_cast<uint16_t>(partition - firstSkipped) < count;
}

// The vector's three pointers must describe whole 16-byte entries, in order, and a count a registry can plausibly hold
// (the PC game has a few thousand). A torn read (the loader grows the vector under its own lock, which we do not take)
// or a changed layout fails this instead of being walked.
inline bool plausibleTable(uintptr_t begin, uintptr_t end, uintptr_t cap, size_t maxCount = 1u << 20) {
    if (!begin || end < begin || cap < end || (begin & 7) || ((end - begin) & 15)) return false;
    return (end - begin) / 16 <= maxCount;
}

// Reads the whole table. False (nothing in `out`) when it is not readable or not plausible. A snapshot: it can be
// slightly stale or, while a level loads, torn; callers check each object before using it.
bool objects(std::vector<Object>& out);

} // namespace mecmp::game::registry
