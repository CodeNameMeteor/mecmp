#include "game/data_registry.h"

#include "game/engine.h"

namespace mecmp::game::registry {

bool objects(std::vector<Object>& out) {
    out.clear();
    uintptr_t t[3] = {};
    if (!mem::read(rva(Registry_Objects), t, sizeof(t)) || !plausibleTable(t[0], t[1], t[2])) return false;
    const size_t n = (t[1] - t[0]) / 16;
    out.resize(n);
    if (n && !mem::read(t[0], out.data(), n * 16)) {
        out.clear();
        return false;
    }
    return true;
}

} // namespace mecmp::game::registry
