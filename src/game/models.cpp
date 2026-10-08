#include "game/models.h"

#include <cctype>
#include <cstdlib>
#include <map>
#include <mutex>

#include "core/log.h"
#include "game/engine.h"
#include "game/level.h"

namespace mecmp::game::models {
namespace {
struct Model { std::string name; uintptr_t ccd = 0; };
std::mutex g_m;
std::vector<Model> g_models;

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string assetName(uintptr_t asset) {
    std::string n;
    mem::readCString(mem::readOr<uintptr_t>(asset + Asset_Name), n);
    return n;
}

// The VisualUnlockAssets on a server character: EASTL set at customization component +0x58
// (anchor +0x60, leftmost +0x68, value at node+0x20).
bool readVisualSet(void* character, std::vector<uintptr_t>& out) {
    void* comp = getComponent(character, TI_ServerCharacterCustomizationComponent);
    if (!comp) return false;
    const uintptr_t set = reinterpret_cast<uintptr_t>(comp) + 0x58;
    const uintptr_t anchor = set + 8;
    uintptr_t node = mem::readOr<uintptr_t>(set + 0x10);
    auto next = reinterpret_cast<uintptr_t (*)(uintptr_t)>(rva(Fn_RbTreeIncrement));
    for (int guard = 0; node && node != anchor && guard < 128; ++guard) {
        if (const uintptr_t v = mem::readOr<uintptr_t>(node + 0x20)) out.push_back(v);
        node = next(node);
    }
    return !out.empty();
}

// First piece of every VisualGroup of a CustomizeCharacterData (+0x18: Array<Array<VisualUnlockAsset*>>).
void visualsOf(uintptr_t ccd, std::vector<uintptr_t>& out) {
    const uintptr_t groups = mem::readOr<uintptr_t>(ccd + 0x18);
    const uint32_t ng = groups ? mem::readOr<uint32_t>(groups - 4) : 0;
    for (uint32_t i = 0; i < ng && i < 64; ++i) {
        const uintptr_t g = mem::readOr<uintptr_t>(groups + i * 8);
        if (g && mem::readOr<uint32_t>(g - 4) > 0)
            if (const uintptr_t v = mem::readOr<uintptr_t>(g)) out.push_back(v);
    }
}

// Our own CustomizeCharacterData: one group per piece (so the game's random pick is that piece),
// no unlocks, ClearVisualState = 1. Never freed (the game may keep pointers to it).
uintptr_t makeCustomization(const std::vector<uintptr_t>& visuals, uintptr_t vtableFrom) {
    auto block = [](size_t n) { return static_cast<uint8_t*>(calloc(1, n)); };
    uint8_t* ccd = block(0x40);
    uint8_t* groups = block(8 + visuals.size() * 8);
    uint8_t* unlocks = block(16);
    if (!ccd || !groups || !unlocks) return 0;
    *reinterpret_cast<uint32_t*>(groups + 4) = static_cast<uint32_t>(visuals.size());
    for (size_t i = 0; i < visuals.size(); ++i) {
        uint8_t* g = block(16);
        if (!g) return 0;
        *reinterpret_cast<uint32_t*>(g + 4) = 1;
        *reinterpret_cast<uintptr_t*>(g + 8) = visuals[i];
        *reinterpret_cast<uintptr_t*>(groups + 8 + i * 8) = reinterpret_cast<uintptr_t>(g + 8);
    }
    if (vtableFrom) *reinterpret_cast<uintptr_t*>(ccd) = mem::readOr<uintptr_t>(vtableFrom);
    static const char kName[] = "MECMP/GhostModel";
    *reinterpret_cast<const char**>(ccd + Asset_Name) = kName;
    *reinterpret_cast<uintptr_t*>(ccd + 0x18) = reinterpret_cast<uintptr_t>(groups + 8);
    *reinterpret_cast<uintptr_t*>(ccd + 0x20) = reinterpret_cast<uintptr_t>(unlocks + 8);
    ccd[0x31] = 1; // ClearVisualState
    return reinterpret_cast<uintptr_t>(ccd);
}

// First-person duplicates of body parts ("..._1p"): not drawn in 3rd person, and they win their slot.
bool isFirstPersonPiece(uintptr_t v) {
    std::string leaf = lower(assetName(v));
    const size_t slash = leaf.find_last_of('/');
    if (slash != std::string::npos) leaf = leaf.substr(slash + 1);
    return (leaf.size() >= 2 && leaf.compare(leaf.size() - 2, 2, "1p") == 0) || leaf.find("1p_") != std::string::npos;
}
} // namespace

void noteTemplate(uintptr_t spawnerData, const std::string& spawnerName) {
    const uintptr_t tmpl = mem::readOr<uintptr_t>(spawnerData + SpawnerData_Template);
    const uintptr_t ccd = tmpl ? mem::readOr<uintptr_t>(tmpl + Template_Customization) : 0;
    if (!ccd) return;
    std::string name = assetName(ccd);
    if (name.empty()) name = spawnerName;
    if (name.find("ErrorTemplate") != std::string::npos) return; // the game's placeholder bodies
    // The ghost is animated on Faith's skeleton. Looks made for other skeletons (enemies,
    // civilians) deform badly on it, so only Faith outfits and the projection looks are offered.
    {
        const std::string ln = lower(name);
        if (ln.find("faith") == std::string::npos && ln.find("projection") == std::string::npos) return;
    }
    std::lock_guard<std::mutex> lock(g_m);
    for (auto& m : g_models)
        if (m.ccd == ccd) return;
    if (g_models.size() >= 64) return;
    g_models.push_back({name, ccd});
    VLOG("model: found '%s'", name.c_str());
}

void reset() {
    std::lock_guard<std::mutex> lock(g_m);
    g_models.clear();
}

std::vector<std::string> names() {
    std::vector<std::string> v{"faith"};
    std::lock_guard<std::mutex> lock(g_m);
    for (auto& m : g_models) v.push_back(m.name);
    return v;
}

bool apply(void* ghost, const std::string& wanted, std::string* usedName) {
    const std::string w = lower(wanted.empty() ? std::string("faith") : wanted);
    std::vector<uintptr_t> visuals;
    uintptr_t vtableFrom = 0;
    std::string used;
    if (w == "faith") {
        void* player = level::playerCharacter();
        if (!player || !readVisualSet(player, visuals)) {
            VLOG("model: could not read Faith's outfit from the player character");
            return false;
        }
        used = "faith";
    } else {
        std::lock_guard<std::mutex> lock(g_m);
        for (auto& m : g_models) {
            if (lower(m.name).find(w) == std::string::npos) continue;
            visualsOf(m.ccd, visuals);
            if (visuals.empty()) continue;
            vtableFrom = m.ccd;
            used = m.name;
            break;
        }
        if (visuals.empty()) return false;
    }
    std::vector<uintptr_t> kept;
    for (uintptr_t v : visuals)
        if (!isFirstPersonPiece(v)) kept.push_back(v);
    if (!kept.empty()) visuals.swap(kept);
    if (!vtableFrom) {
        std::lock_guard<std::mutex> lock(g_m);
        if (!g_models.empty()) vtableFrom = g_models.front().ccd;
    }
    static std::map<std::vector<uintptr_t>, uintptr_t> cache;
    uintptr_t& ccd = cache[visuals];
    if (!ccd) ccd = makeCustomization(visuals, vtableFrom);
    if (!ccd) return false;

    const uintptr_t ch = reinterpret_cast<uintptr_t>(ghost);
    uintptr_t vt = 0, fn = 0;
    if (!mem::read(ch, vt) || !inImage(vt) || !mem::read(vt + Vtbl_ApplyCustomize, fn) || !inImage(fn)) return false;
    // Ghost shader off for normal meshes, on for the projection looks.
    {
        const uint8_t ghostShader = lower(used).find("projection") != std::string::npos ? 1 : 0;
        const uintptr_t data = mem::readOr<uintptr_t>(ch + Character_Data);
        uint8_t cur = 0;
        if (data && mem::read(data + CharData_GhostShader, cur) && cur != ghostShader) poke(data + CharData_GhostShader, ghostShader);
    }
    struct Args { void* ch; void* player; void* customize; uint8_t flag; uint8_t pad[7]; } args{};
    args.ch = ghost;
    args.customize = reinterpret_cast<void*>(ccd);
    reinterpret_cast<void (*)(void*, Args*)>(fn)(ghost, &args);
    VLOG("model: ghost %p now uses '%s' (%zu pieces)", ghost, used.c_str(), visuals.size());
    if (usedName) *usedName = used;
    return true;
}

} // namespace mecmp::game::models
