#include "game/models.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#include "core/log.h"
#include "game/engine.h"
#include "game/level.h"

namespace mecmp::game::models {
namespace {
struct Model { std::string name; uintptr_t ccd = 0; };
std::mutex g_m;
std::vector<Model> g_models;
// Customizations we built, by piece list. Pieces are level data, so this is emptied when the level
// changes (a new level can put other assets at the same addresses). The blocks are never freed.
std::map<std::pair<std::vector<uintptr_t>, uintptr_t>, uintptr_t> g_cache; // (pieces, unlocks) -> ours
// The game's echo (runner ghost) look catalogue. Published as a whole and never edited in place, so the
// menu can keep a pointer to it while it is replaced.
std::shared_ptr<const std::vector<Echo>> g_echoes = std::make_shared<const std::vector<Echo>>();
// CustomizeCharacterData vtable (in the game image, so valid in every level), from any template seen.
uintptr_t g_ccdVtable = 0;
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

// Our own CustomizeCharacterData: one group per piece (so the game's random pick is that piece), the
// source's unlocks (an echo look's colour is an unlock: an ObjectVariation on the projection shader),
// ClearVisualState = 1. Never freed (the game may keep pointers to it).
uintptr_t makeCustomization(const std::vector<uintptr_t>& visuals, uintptr_t vtable, uintptr_t unlockArray, uintptr_t preset) {
    if (!vtable) return 0; // never hand the game a data object without a vtable
    auto block = [](size_t n) { return static_cast<uint8_t*>(calloc(1, n)); };
    uint8_t* ccd = block(0x70);
    if (!ccd) return 0;
    // Pam presets carry material and animation parameters beyond the base customization.
    if (preset && vtable == rva(0x141c883e0) && !mem::read(preset, ccd, 0x70)) { free(ccd); return 0; }
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
    *reinterpret_cast<uintptr_t*>(ccd) = vtable;
    static const char kName[] = "MECMP/GhostModel";
    *reinterpret_cast<const char**>(ccd + Asset_Name) = kName;
    *reinterpret_cast<uintptr_t*>(ccd + 0x18) = reinterpret_cast<uintptr_t>(groups + 8);
    *reinterpret_cast<uintptr_t*>(ccd + 0x20) = unlockArray ? unlockArray : reinterpret_cast<uintptr_t>(unlocks + 8);
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
    if (!g_ccdVtable) {
        const uintptr_t vt = mem::readOr<uintptr_t>(ccd);
        if (inImage(vt)) g_ccdVtable = vt;
    }
    for (auto& m : g_models)
        if (m.ccd == ccd) return;
    if (g_models.size() >= 64) return;
    g_models.push_back({name, ccd});
    VLOG("model: found '%s'", name.c_str());
}

void reset() {
    std::lock_guard<std::mutex> lock(g_m);
    g_models.clear();
    g_cache.clear();
    g_echoes = std::make_shared<const std::vector<Echo>>(); // level data may go with the level: found again by findEchoes()
}

namespace {
// Builds (or reuses) our customization from these pieces and unlocks and applies it to the ghost.
bool applyPieces(void* ghost, std::vector<uintptr_t> visuals, uintptr_t unlockArray, uintptr_t vtableFrom,
                 const std::string& used) {
    std::vector<uintptr_t> kept;
    for (uintptr_t v : visuals)
        if (!isFirstPersonPiece(v)) kept.push_back(v);
    if (!kept.empty()) visuals.swap(kept);
    uintptr_t ccd = 0;
    {
        std::lock_guard<std::mutex> lock(g_m);
        uintptr_t vtable = vtableFrom ? mem::readOr<uintptr_t>(vtableFrom) : 0;
        if (!inImage(vtable)) vtable = g_ccdVtable;
        const auto key = std::make_pair(visuals, unlockArray);
        auto it = g_cache.find(key);
        if (it != g_cache.end()) {
            ccd = it->second;
        } else if ((ccd = makeCustomization(visuals, vtable, unlockArray, vtableFrom)) != 0) {
            g_cache[key] = ccd;
        } else {
            MLOG("model: cannot build '%s' yet (no customization template seen in this game session)", used.c_str());
            return false;
        }
    }

    const uintptr_t ch = reinterpret_cast<uintptr_t>(ghost);
    uintptr_t vt = 0, fn = 0;
    if (!mem::read(ch, vt) || !inImage(vt) || !mem::read(vt + Vtbl_ApplyCustomize, fn) || !inImage(fn)) return false;
    // Arguments of vtbl[0x340] (0x14336cf00): +0x00 character; +0x08 player (null: apply the data
    // as is via 0x14336c860, non-null: the player's own unlock path); +0x10 CustomizeCharacterData
    // (reads +0x18 groups, +0x20 unlocks, +0x28 a fade time, +0x30/+0x31 flags); +0x18 non-zero =
    // do nothing. Checked in Ghidra (B-12).
    struct Args { void* ch; void* player; void* customize; uint8_t skip; uint8_t pad[7]; } args{};
    static_assert(offsetof(Args, player) == 0x08 && offsetof(Args, customize) == 0x10 && offsetof(Args, skip) == 0x18,
                  "customize args");
    args.ch = ghost;
    args.customize = reinterpret_cast<void*>(ccd);
    reinterpret_cast<void (*)(void*, Args*)>(fn)(ghost, &args);
    VLOG("model: ghost %p now uses '%s' (%llu pieces)", ghost, used.c_str(), static_cast<unsigned long long>(visuals.size()));
    return true;
}

// "Characters/Customizations/Projection/RunVis/Customization_Projection_RunVis_Red" -> RunVis, Red.
const char* const kColours[] = {"Black", "Blue", "Green", "Orange", "Red", "Teal", "Yellow", "White", "Purple", "Pink", "Grey", "Gray"};
void splitEchoName(const std::string& full, std::string& style, std::string& colour) {
    std::string leaf = full.substr(full.find_last_of('/') == std::string::npos ? 0 : full.find_last_of('/') + 1);
    for (const char* prefix : {"Customization_Projection_", "Customization_Echo_", "Customization_"})
        if (leaf.rfind(prefix, 0) == 0) { leaf = leaf.substr(strlen(prefix)); break; }
    colour = "Default";
    const size_t us = leaf.find_last_of('_');
    if (us != std::string::npos)
        for (const char* c : kColours)
            if (_stricmp(leaf.c_str() + us + 1, c) == 0) { colour = c; leaf.resize(us); break; }
    style = leaf.empty() ? std::string("Ghost") : leaf;
}
} // namespace

bool apply(void* ghost, const std::string& wanted, std::string* usedName) {
    const std::string w = lower(wanted.empty() ? std::string("faith") : wanted);
    std::vector<uintptr_t> visuals;
    uintptr_t vtableFrom = 0, unlocks = 0;
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
            unlocks = mem::readOr<uintptr_t>(m.ccd + 0x20);
            used = m.name;
            break;
        }
        if (visuals.empty()) return false;
    }
    if (!applyPieces(ghost, visuals, unlocks, vtableFrom, used))
        return false;
    if (usedName) *usedName = used;
    return true;
}

std::shared_ptr<const std::vector<Echo>> echoes() {
    std::lock_guard<std::mutex> lock(g_m);
    return g_echoes;
}

// An echo look's colour and pattern are unlocks on the projection body. Some echo customizations
// carry no body piece of their own; built on their own (ClearVisualState = 1) the ghost then had no
// mesh at all and went invisible. Those get the projection body pieces (RunVis) under their unlocks.
bool applyEcho(void* ghost, uint32_t hash) {
    Echo e;
    std::vector<uintptr_t> visuals, base;
    {
        std::lock_guard<std::mutex> lock(g_m);
        const auto& list = *g_echoes;
        auto it = std::find_if(list.begin(), list.end(), [hash](const Echo& x) { return x.hash == hash; });
        if (it == list.end()) return false;
        e = *it;
        for (auto& m : g_models)
            if (lower(m.name).find("projection/runvis") != std::string::npos) { visualsOf(m.ccd, base); break; }
    }
    visualsOf(e.ccd, visuals);
    const size_t own = visuals.size();
    if (visuals.empty()) visuals = base;
    if (visuals.empty()) { // no RunVis template seen: keep the projection pieces the ghost has now
        std::vector<uintptr_t> cur;
        if (readVisualSet(ghost, cur))
            for (uintptr_t v : cur)
                if (lower(assetName(v)).find("projection") != std::string::npos) visuals.push_back(v);
    }
    const uintptr_t unlocks = mem::readOr<uintptr_t>(e.ccd + 0x20);
    MLOG("model: echo %s/%s: %llu own pieces, %llu used, %u unlocks", e.style.c_str(), e.colour.c_str(),
         static_cast<unsigned long long>(own), static_cast<unsigned long long>(visuals.size()),
         unlocks ? mem::readOr<uint32_t>(unlocks - 4) : 0u);
    if (visuals.empty()) return false; // nothing to build a body from: leave the ghost as it is
    return applyPieces(ghost, visuals, unlocks, e.ccd, e.name);
}

bool echoRenders(uint32_t hash) {
    std::lock_guard<std::mutex> lock(g_m);
    for (const auto& e : *g_echoes) if (e.hash == hash) return true;
    return false;
}

uint32_t defaultEcho() {
    std::lock_guard<std::mutex> lock(g_m);
    uint32_t firstRunVis = 0;
    const auto& list = *g_echoes;
    for (const auto& e : list)
        if (_stricmp(e.style.c_str(), "RunVis") == 0) {
            if (_stricmp(e.colour.c_str(), "Red") == 0) return e.hash;
            if (!firstRunVis) firstRunVis = e.hash;
        }
    if (firstRunVis) return firstRunVis;
    return list.empty() ? 0 : list.front().hash;
}

// PamServerCustomizeProjectionCharacterEntity (time-trial markers; vtable 0x141c7ea80): +0x28 its data,
// data +0x20 PamEchoCustomizationDefinitionsMeta, meta +0x18 Array<PamEchoCustomizationData> (0x28
// each: +0x18 CustomizeCharacterData*, +0x20 u32 CustomizationHash). (See Ghidra: ..._vf4 0x143a83e70.)
bool findEchoes() {
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (!g_echoes->empty()) return true;
    }
    const uintptr_t vt = rva(Vtbl_PamServerCustomizeProjectionCharacterEntity);
    std::vector<uint8_t> buf(4 << 20);
    std::vector<Echo> found;
    MEMORY_BASIC_INFORMATION mbi{};
    const DWORD start = GetTickCount();
    for (uintptr_t addr = 0x10000; found.empty() && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == sizeof(mbi);
         addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || mbi.Protect != PAGE_READWRITE) continue;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        for (size_t off = 0; found.empty() && off < mbi.RegionSize; off += buf.size()) {
            const size_t n = std::min(buf.size(), static_cast<size_t>(mbi.RegionSize - off));
            if (!mem::read(base + off, buf.data(), n)) continue;
            const auto* q = reinterpret_cast<const uintptr_t*>(buf.data());
            for (size_t i = 0; found.empty() && i < n / 8; ++i) {
                if (q[i] != vt) continue;
                const uintptr_t ent = base + off + i * 8;
                const uintptr_t data = mem::readOr<uintptr_t>(ent + 0x28);
                const uintptr_t meta = data ? mem::readOr<uintptr_t>(data + 0x20) : 0;
                const uintptr_t arr = meta ? mem::readOr<uintptr_t>(meta + 0x18) : 0;
                const uint32_t count = arr ? mem::readOr<uint32_t>(arr - 4) : 0;
                if (!count || count > 512) continue;
                for (uint32_t k = 0; k < count; ++k) {
                    const uintptr_t e = arr + k * 0x28;
                    Echo x;
                    x.ccd = mem::readOr<uintptr_t>(e + 0x18);
                    x.hash = mem::readOr<uint32_t>(e + 0x20);
                    if (!x.ccd || !x.hash || !mem::readCString(mem::readOr<uintptr_t>(x.ccd + Asset_Name), x.name)) continue;
                    splitEchoName(x.name, x.style, x.colour);
                    found.push_back(x);
                }
            }
        }
    }
    if (found.empty()) {
        MLOG("model: no ghost look list found yet (%.1f s)", (GetTickCount() - start) / 1000.0);
        return false;
    }
    std::sort(found.begin(), found.end(), [](const Echo& a, const Echo& b) {
        return a.style != b.style ? a.style < b.style : a.colour < b.colour;
    });
    MLOG("model: %llu ghost looks found (%.1f s), e.g. %s = %08x", static_cast<unsigned long long>(found.size()),
         (GetTickCount() - start) / 1000.0, found.front().name.c_str(), found.front().hash);
    std::lock_guard<std::mutex> lock(g_m);
    g_echoes = std::make_shared<const std::vector<Echo>>(std::move(found));
    return true;
}

} // namespace mecmp::game::models
