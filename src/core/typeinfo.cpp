#include "typeinfo.h"

#include "mem.h"

namespace mecmp {

namespace {
constexpr size_t kMaxTypes = 50000; // guard against a corrupt or cyclic list
}

size_t TypeList::countFrom(uintptr_t listHeadAddr) {
    uintptr_t ti = 0;
    if (!mem::read(listHeadAddr, ti)) return 0;
    size_t n = 0;
    while (ti && n < kMaxTypes) {
        uintptr_t next = 0;
        if (!mem::read(ti + 8, next)) break;
        ++n;
        ti = next;
    }
    return n;
}

void TypeList::load(uintptr_t listHeadAddr) {
    entries_.clear();
    byName_.clear();
    uintptr_t ti = 0;
    if (!mem::read(listHeadAddr, ti)) return;
    while (ti && entries_.size() < kMaxTypes) {
        TypeEntry e;
        e.ti = ti;
        mem::read(ti, e.data);
        if (e.data) {
            uintptr_t namePtr = 0;
            if (mem::read(e.data, namePtr) && namePtr) mem::readCString(namePtr, e.name);
            mem::read(e.data + 8, e.flags);
            mem::read(e.data + 0xA, e.size);
        }
        if (!e.name.empty()) byName_.emplace(e.name, entries_.size()); // first wins
        entries_.push_back(std::move(e));
        uintptr_t next = 0;
        if (!mem::read(ti + 8, next)) break;
        ti = next;
    }
}

const TypeEntry* TypeList::find(const std::string& name) const {
    auto it = byName_.find(name);
    return it == byName_.end() ? nullptr : &entries_[it->second];
}

} // namespace mecmp
