#include "game/models.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#include "core/config.h"
#include "core/log.h"
#include "game/engine.h"
#include "game/level.h"
#include "net/protocol.h"

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

// Every look piece (VisualUnlockAsset) loaded in this level, by lower-case name: what an outfit or the
// player's own look is built from. Found by findPieces() once something asks for it (g_piecesNeeded).
// Guarded by g_m; level data.
std::map<std::string, uintptr_t> g_pieces;
bool g_piecesKnown = false;
// False if RenderIn1p was not where it is expected in this level's pieces: no first-person twins then.
bool g_twinsOk = false;
std::atomic<bool> g_piecesNeeded{false};
std::atomic<uintptr_t> g_pieceVtable{0}; // what makes an object a look piece (from the player's own)
// The player's own look, as asked for (setOwnLook): echo hash in the low 32 bits, then the outfit,
// bit 40 = on at all.
std::atomic<uint64_t> g_ownWanted{0};
std::atomic<bool> g_ownForget{false}; // level changed: what tick() remembers is gone
std::atomic<unsigned> g_levelCount{0}; // goes up with every reset(): a piece scan across one is thrown away
// The pieces the game itself gave the player (before our change), for the "faith" ghosts of the others.
std::vector<uintptr_t> g_ownOriginal; // guarded by g_m
// Pieces seen on the player character as the game dressed it: certainly the live assets (findPieces()
// tells them from leftovers in memory by this). Guarded by g_m; level data.
std::vector<uintptr_t> g_livePieces;
void noteLivePieces(const std::vector<uintptr_t>& pieces) {
    std::lock_guard<std::mutex> lock(g_m);
    for (uintptr_t v : pieces)
        if (std::find(g_livePieces.begin(), g_livePieces.end(), v) == g_livePieces.end() && g_livePieces.size() < 256)
            g_livePieces.push_back(v);
}

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
    uint8_t* ccd = block(PamCustomizeCharacterData_Size);
    if (!ccd) return 0;
    // Pam presets carry material and animation parameters beyond the base customization.
    if (preset && vtable == rva(Vtbl_PamCustomizeCharacterData) && !mem::read(preset, ccd, PamCustomizeCharacterData_Size)) {
        free(ccd);
        return 0;
    }
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
    g_pieces.clear();
    g_piecesKnown = false;
    g_ownOriginal.clear();
    g_livePieces.clear();
    g_ownForget = true;
    ++g_levelCount;
    g_echoes =std::make_shared<const std::vector<Echo>>(); // level data may go with the level: found again by findEchoes()
}

namespace {
// Builds (or reuses) our customization from these pieces and unlocks and applies it to the ghost.
// `keepFirstPerson`: for the player's own character, which is seen from inside.
bool applyPieces(void* ghost, std::vector<uintptr_t> visuals, uintptr_t unlockArray, uintptr_t vtableFrom,
                 const std::string& used, bool keepFirstPerson = false) {
    std::vector<uintptr_t> kept;
    for (uintptr_t v : visuals)
        if (keepFirstPerson || !isFirstPersonPiece(v)) kept.push_back(v);
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
    VLOG("model: character %p now uses '%s' (%llu pieces)", ghost, used.c_str(), static_cast<unsigned long long>(visuals.size()));
    return true;
}

// --- Faith's outfits ------------------------------------------------------------------------------
// All of these are pieces the player's spawner refers to, so they are loaded in free roam.
namespace wear = net::protocol; // the kWear... bits
const char* const kPrologue[] = { // its sleeves and gloves are part of the upper body: no arm pieces
    "characters/faith/hair/unlock_faith_hair_3pcloth_ingame",
    "characters/faith_prologue/head/unlock_faith_head_prologue",
    "characters/faith_prologue/lowerbody/unlock_faith_prologue_lowerbody",
    "characters/faith_prologue/upperbody/unlock_faith_prologue_upperbody",
    "characters/faith/headparts/unlock_faith_headparts",
    "characters/faith_prologue/lowerbody/unlock_faith_prologue_lowerbody1p",
    "characters/faith_prologue/upperbody/unlock_faith_prologue_upperbody1p",
};
const char* const kGridLink[] = {
    "characters/faith/gridlink/unlock_faith_gridlink",
    "characters/faith/gridlink/unlock_faith_gridlink_1p",
};
struct Swap { const char* from; const char* to; };
const Swap kTattoo[] = {
    {"characters/faith/arms/unlock_faith_arms_notattoo", "characters/faith/arms/unlock_faith_arms"},
    {"characters/faith/arms/unlock_faith_arms1p_notattoo", "characters/faith/arms/unlock_faith_arms1p"},
};

// Faith's gear, by lower-case piece name: worn over any outfit of hers.
bool isMagRope(const std::string& name) { return name.find("/armgear/magrope/") != std::string::npos; }
bool isToolbag(const std::string& name) { return name.find("/toolbag/") != std::string::npos; }
bool isGear(const std::string& name) { return isMagRope(name) || isToolbag(name); }

// g_m held. 0 if that piece is not loaded.
uintptr_t pieceLocked(const char* name) {
    const auto it = g_pieces.find(name);
    return it == g_pieces.end() ? 0 : it->second;
}

// `base` (the pieces the game gave the player) with the outfit changes `worn` (kWear... bits) made.
// False while the piece list is not known yet (asked for here; findPieces() gets it), or a piece that
// is needed is not loaded.
bool outfitPieces(const std::vector<uintptr_t>& base, uint8_t worn, std::vector<uintptr_t>& out) {
    out = base;
    if (!worn) return true;
    // Taking pieces off needs no list: they are known by their names.
    if (worn & (wear::kWearNoMagRope | wear::kWearNoToolbag))
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [worn](uintptr_t v) {
                                     const std::string n = lower(assetName(v));
                                     return ((worn & wear::kWearNoMagRope) && isMagRope(n)) || ((worn & wear::kWearNoToolbag) && isToolbag(n));
                                 }),
                  out.end());
    if (!(worn & (wear::kWearPrologue | wear::kWearTattoo | wear::kWearNoTattoo | wear::kWearGridLink))) return !out.empty();
    g_piecesNeeded = true;
    std::lock_guard<std::mutex> lock(g_m);
    if (!g_piecesKnown) return false;
    if (worn & wear::kWearPrologue) {
        // The gear the player has (MagRope, tool bag; what is left of it after the above) stays on.
        out.erase(std::remove_if(out.begin(), out.end(), [](uintptr_t v) { return !isGear(lower(assetName(v))); }), out.end());
        for (const char* name : kPrologue) {
            const uintptr_t v = pieceLocked(name);
            if (!v) return false;
            out.push_back(v);
        }
    } else if (worn & (wear::kWearTattoo | wear::kWearNoTattoo)) {
        const bool tattoo = (worn & wear::kWearTattoo) != 0;
        for (const Swap& s : kTattoo) {
            const uintptr_t a = pieceLocked(tattoo ? s.from : s.to);
            const uintptr_t b = pieceLocked(tattoo ? s.to : s.from);
            if (!a || !b) return false;
            std::replace(out.begin(), out.end(), a, b);
        }
    }
    if (worn & wear::kWearGridLink)
        for (const char* name : kGridLink) {
            const uintptr_t v = pieceLocked(name);
            if (!v) return false;
            if (std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
        }
    return !out.empty();
}

// The pieces the game itself gave the player: what was there before our own change, if there was one.
bool playerBasePieces(std::vector<uintptr_t>& out) {
    {
        std::lock_guard<std::mutex> lock(g_m);
        out = g_ownOriginal;
    }
    if (!out.empty()) return true;
    void* player = level::playerCharacter();
    if (!player || !readVisualSet(player, out)) return false;
    noteLivePieces(out);
    if (!g_pieceVtable.load()) { // findPieces() needs it
        const uintptr_t vt = mem::readOr<uintptr_t>(out.front());
        if (inImage(vt)) g_pieceVtable = vt;
    }
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
    if (w == "faith" || w.rfind("faith:", 0) == 0) {
        std::vector<uintptr_t> base;
        if (!playerBasePieces(base)) {
            MLOG("model: could not read Faith's outfit from the player character");
            return false;
        }
        const uint8_t outfit = w.size() > 6 ? static_cast<uint8_t>(strtoul(w.c_str() + 6, nullptr, 10)) : 0;
        if (!outfitPieces(base, outfit, visuals)) return false; // its pieces are not known yet: tried again
        used = w;
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

// --- The player's own look --------------------------------------------------------------------------
namespace {
constexpr uint64_t kOwnOn = 1ull << 40;
constexpr DWORD kOwnCheckMs = 500;  // how often the player's pieces are compared with what was asked for
constexpr DWORD kOwnRetryMs = 2000; // a change that could not be made yet is tried again after this

// Game thread only.
struct Own {
    void* character = nullptr;      // the player character all of this is about
    std::vector<uintptr_t> applied; // sorted: what the player wore right after our change
    uint64_t key = 0;               // the wish that change was made for; 0 = the game's own look
    uint64_t triedKey = 0;
    DWORD triedAt = 0, checkedAt = 0;
    // The placeholder pieces we wrote first-person twins into, with their own contents.
    std::map<uintptr_t, std::array<uint8_t, VisualUnlockAsset_Size>> donors;
} g_own;

void notePieceVtable(uintptr_t piece) {
    if (g_pieceVtable.load()) return;
    const uintptr_t vt = mem::readOr<uintptr_t>(piece);
    if (inImage(vt)) g_pieceVtable = vt;
}

// An echo look's body pieces and unlocks, as the game's own data has them; the plain projection body
// (RunVis) for a look that carries none.
bool echoPieces(uint32_t hash, Echo& e, std::vector<uintptr_t>& visuals, uintptr_t& unlocks) {
    std::vector<uintptr_t> base;
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
    if (visuals.empty()) visuals = base;
    unlocks = mem::readOr<uintptr_t>(e.ccd + 0x20);
    return !visuals.empty();
}

// The runner ghost shader on the player's own client character: echo pieces are invisible without it,
// normal ones with it. True if it had to be changed.
bool setOwnGhostShader(bool on) {
    const uintptr_t client = reinterpret_cast<uintptr_t>(reinterpret_cast<void* (*)()>(rva(Fn_LocalClientCharacter))());
    uint8_t now = 0;
    if (!client || mem::readOr<uintptr_t>(client) != rva(Vtbl_PamClientCharacterEntity) ||
        !mem::read(client + ClientChar_GhostShader, now) || now == static_cast<uint8_t>(on))
        return false;
    poke<uint8_t>(client + ClientChar_GhostShader, on ? 1 : 0);
    return true;
}

void restoreDonors() {
    const uintptr_t vt = g_pieceVtable.load();
    for (const auto& d : g_own.donors)
        if (mem::readOr<uintptr_t>(d.first) == vt)
            poke(d.first + DataContainer_Header, d.second.data() + DataContainer_Header, VisualUnlockAsset_Size - DataContainer_Header);
    g_own.donors.clear();
}

// A piece is drawn in one view only (VisualUnlockAsset::RenderIn1p), which is why Faith's body parts
// come in pairs ("..._1p"). Pieces without such a pair (the echo bodies) would leave the player with no
// body of their own, so each gets a first-person twin: its contents, with the flag set, written into
// one of the game's placeholder pieces ("ErrorTemplate", worn by nobody) and worn beside it. It has to
// be a piece of the game's own: the look reaches the client through the level's network registry, and
// a copy of ours made the whole character disappear (tried in game).
void addFirstPersonTwins(std::vector<uintptr_t>& pieces) {
    std::vector<uintptr_t> spare;
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (!g_twinsOk) return;
        for (const auto& p : g_pieces)
            if (p.first.find("errortemplate") != std::string::npos && std::find(pieces.begin(), pieces.end(), p.second) == pieces.end())
                spare.push_back(p.second);
    }
    const uintptr_t vt = g_pieceVtable.load();
    const std::vector<uintptr_t> asked = pieces;
    size_t used = 0;
    for (uintptr_t v : asked) {
        if (mem::readOr<uint8_t>(v + VisualUnlock_RenderIn1p, 1) != 0) continue;
        if (used >= spare.size()) {
            MLOG("look: no placeholder piece left for a first-person twin of %s", assetName(v).c_str());
            break;
        }
        const uintptr_t donor = spare[used++];
        std::array<uint8_t, VisualUnlockAsset_Size> body{}, own{};
        if (mem::readOr<uintptr_t>(v) != vt || mem::readOr<uintptr_t>(donor) != vt || !mem::read(v, body.data(), body.size()) ||
            !mem::read(donor, own.data(), own.size()))
            continue;
        g_own.donors.emplace(donor, own); // keeps the first (the real) contents
        body[VisualUnlock_RenderIn1p] = 1;
        poke(donor + DataContainer_Header, body.data() + DataContainer_Header, VisualUnlockAsset_Size - DataContainer_Header);
        pieces.push_back(donor);
    }
}

void setOriginal(std::vector<uintptr_t> pieces) {
    std::lock_guard<std::mutex> lock(g_m);
    g_ownOriginal = std::move(pieces);
}
} // namespace

void setOwnLook(uint32_t echoHash, uint8_t outfit, bool enabled) {
    g_ownWanted = (enabled ? kOwnOn : 0) | (static_cast<uint64_t>(echoHash ? 0 : outfit) << 32) | echoHash;
}

void tick() {
    if (g_ownForget.exchange(false)) g_own = Own{};
    const uint64_t want = g_ownWanted.load();
    // What to make of the player: nothing while it is off or simply "Faith, as in the game".
    const uint64_t wish = (want & kOwnOn) && (want & ~kOwnOn) ? want : 0;
    if (!wish && !g_own.key) return;
    const DWORD nowTick = GetTickCount();
    if (nowTick - g_own.checkedAt < kOwnCheckMs) return;
    g_own.checkedAt = nowTick;
    void* player = level::playerCharacter();
    std::vector<uintptr_t> now;
    if (!player || !readVisualSet(player, now)) return;
    notePieceVtable(now.front());
    std::sort(now.begin(), now.end());

    std::vector<uintptr_t> original;
    {
        std::lock_guard<std::mutex> lock(g_m);
        original = g_ownOriginal;
    }
    if (player != g_own.character) { // a new character (respawn): it wears the game's own look again
        g_own.character = player;
        g_own.key = 0;
        g_own.applied.clear();
        original.clear();
    }
    if (g_own.key && now != g_own.applied) {
        // The game changed the look itself (story progress, a mission): that is the look to build on.
        // If it only added to ours, our pieces do not count.
        auto in = [](const std::vector<uintptr_t>& v, uintptr_t x) { return std::find(v.begin(), v.end(), x) != v.end(); };
        bool mixed = false;
        for (uintptr_t v : now) mixed = mixed || (in(g_own.applied, v) && !in(original, v));
        if (mixed) {
            for (uintptr_t v : now)
                if (!in(g_own.applied, v) && !in(original, v)) original.push_back(v);
        } else {
            original = now;
        }
        g_own.key = 0;
        g_own.triedKey = 0;
        setOriginal(original);
        VLOG("look: the game changed your look; yours is put back on top of it");
    }
    if (!g_own.key && original.empty()) { // before the first change: this is the game's own look
        original = now;
        setOriginal(original);
    }
    noteLivePieces(original);

    const bool echo = static_cast<uint32_t>(wish) != 0;
    if (wish == g_own.key) {
        // A client character made after the change (respawn) starts without the shader: redo the look.
        if (echo && setOwnGhostShader(true)) g_own.key = 0;
        return;
    }
    if (wish == g_own.triedKey && nowTick - g_own.triedAt < kOwnRetryMs) return;
    g_own.triedKey = wish;
    g_own.triedAt = nowTick;

    std::vector<uintptr_t> pieces;
    uintptr_t unlocks = 0, preset = 0;
    std::string name = "faith";
    if (echo) {
        Echo e;
        if (!echoPieces(static_cast<uint32_t>(wish), e, pieces, unlocks)) return; // the look list is not here yet
        g_piecesNeeded = true;
        {
            std::lock_guard<std::mutex> lock(g_m);
            if (!g_piecesKnown) return; // the twins need the piece list: findPieces() is getting it
        }
        addFirstPersonTwins(pieces);
        preset = e.ccd;
        name = e.name;
    } else if (wish) {
        const uint8_t outfit = static_cast<uint8_t>(wish >> 32);
        if (!outfitPieces(original, outfit, pieces)) return;
        name = "faith:" + std::to_string(outfit);
    } else {
        pieces = original;
    }
    if (!preset) { // our own data object: a game preset is copied under it (health and material fields)
        std::lock_guard<std::mutex> lock(g_m);
        for (const auto& m : g_models)
            if (!preset || lower(m.name).find("faith") != std::string::npos) preset = m.ccd;
    }
    if (!preset) return;
    if (!echo) restoreDonors();
    setOwnGhostShader(echo);
    if (!applyPieces(player, pieces, unlocks, preset, name, true)) return;
    now.clear();
    readVisualSet(player, now);
    std::sort(now.begin(), now.end());
    g_own.applied = now;
    g_own.key = wish;
    if (!wish) { // back to the game's own look: nothing of ours is left
        g_own.applied.clear();
        setOriginal({});
    }
    MLOG("look: you now look like '%s' to yourself (%llu pieces)", wish ? name.c_str() : "Faith, as in the game",
         static_cast<unsigned long long>(now.size()));
}

bool findPieces() {
    if (!g_piecesNeeded.load()) return true;
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (g_piecesKnown) return true;
    }
    const uintptr_t vt = g_pieceVtable.load();
    if (!vt) return false; // no player character seen yet
    const unsigned level = g_levelCount.load();
    const DWORD start = GetTickCount();
    // The heap can hold more than one object per name: what an earlier, torn down load of the level left
    // behind looks just like the live asset, but its contents may be anything. So every candidate is
    // collected, and the one to use is picked below.
    std::map<std::string, std::vector<uintptr_t>> seen;
    size_t objects = 0;
    std::vector<uint8_t> buf(4 << 20);
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t addr = 0x10000; objects < 8000 && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == sizeof(mbi);
         addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || mbi.Protect != PAGE_READWRITE) continue;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        for (size_t off = 0; off < mbi.RegionSize; off += buf.size()) {
            const size_t n = std::min(buf.size(), static_cast<size_t>(mbi.RegionSize - off));
            if (!mem::read(base + off, buf.data(), n)) continue;
            const auto* q = reinterpret_cast<const uintptr_t*>(buf.data());
            for (size_t i = 0; i < n / 8; ++i) {
                if (q[i] != vt) continue;
                const std::string name = lower(assetName(base + off + i * 8));
                if (name.find("/unlock_") == std::string::npos) continue;
                seen[name].push_back(base + off + i * 8);
                ++objects;
            }
        }
    }
    // Pieces that are certainly the live ones: what the player wears, and what the looks seen in this
    // level are made of.
    std::vector<uintptr_t> live;
    {
        std::lock_guard<std::mutex> lock(g_m);
        live = g_livePieces;
        for (const auto& m : g_models) visualsOf(m.ccd, live);
        for (const auto& e : *g_echoes) visualsOf(e.ccd, live);
    }
    auto isLive = [&live](uintptr_t v) { return std::find(live.begin(), live.end(), v) != live.end(); };
    // A piece is what its name says: RenderIn1p set in a "..._1p" piece, clear in any other.
    auto sane = [](uintptr_t v) {
        return mem::readOr<uint8_t>(v + VisualUnlock_RenderIn1p, 0xff) == (isFirstPersonPiece(v) ? 1 : 0);
    };
    std::map<std::string, uintptr_t> found;
    size_t doubles = 0, dropped = 0;
    for (const auto& s : seen) {
        if (s.second.size() > 1) ++doubles;
        uintptr_t pick = 0;
        for (uintptr_t v : s.second)
            if (isLive(v)) { pick = v; break; }
        for (size_t i = 0; !pick && i < s.second.size(); ++i)
            if (sane(s.second[i])) pick = s.second[i];
        if (!pick) {
            ++dropped;
            VLOG("look: piece '%s' left out (%llu in memory, none as its name says)", s.first.c_str(),
                 static_cast<unsigned long long>(s.second.size()));
            continue;
        }
        found.emplace(s.first, pick);
    }
    // First-person twins rely on RenderIn1p being where it was found to be: checked on the pieces the
    // player wears (live for certain), which come in both kinds.
    size_t firstPerson = 0, wrong = 0;
    for (uintptr_t v : live) {
        if (mem::readOr<uintptr_t>(v) != vt) continue;
        if (isFirstPersonPiece(v)) ++firstPerson;
        if (!sane(v)) {
            ++wrong;
            MLOG("look: piece '%s' has RenderIn1p %u, not as its name says", assetName(v).c_str(),
                 mem::readOr<uint8_t>(v + VisualUnlock_RenderIn1p, 0xff));
        }
    }
    const bool twins = firstPerson >= 3 && wrong == 0;
    std::lock_guard<std::mutex> lock(g_m);
    if (level != g_levelCount.load()) return false; // the level changed meanwhile: these are gone
    MLOG("look: %llu look pieces are loaded (%.1f s; %llu objects, %llu names more than once, %llu left out)%s",
         static_cast<unsigned long long>(found.size()), (GetTickCount() - start) / 1000.0, static_cast<unsigned long long>(objects),
         static_cast<unsigned long long>(doubles), static_cast<unsigned long long>(dropped),
         twins ? "" : "; first-person flag not as expected, echo looks stay third-person only");
    g_pieces = std::move(found);
    g_piecesKnown = true;
    g_twinsOk = twins;
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

namespace {
// Look-picker entities seen being created (factory hook). Never cleared: a stale entry fails the vtable
// and data checks below, and the newest come first.
std::mutex g_seenM;
std::vector<uintptr_t> g_seen;

// The catalogue of one look-picker entity: entity +0x28 data, data +0x20 meta, meta +0x18 array.
bool readCatalogue(uintptr_t ent, std::vector<Echo>& found) {
    if (mem::readOr<uintptr_t>(ent) != rva(Vtbl_PamServerCustomizeProjectionCharacterEntity)) return false;
    const uintptr_t data = mem::readOr<uintptr_t>(ent + 0x28);
    const uintptr_t meta = data ? mem::readOr<uintptr_t>(data + 0x20) : 0;
    const uintptr_t arr = meta ? mem::readOr<uintptr_t>(meta + 0x18) : 0;
    const uint32_t count = arr ? mem::readOr<uint32_t>(arr - 4) : 0;
    if (!count || count > 512) return false;
    for (uint32_t k = 0; k < count; ++k) {
        const uintptr_t e = arr + k * 0x28;
        Echo x;
        x.ccd = mem::readOr<uintptr_t>(e + 0x18);
        x.hash = mem::readOr<uint32_t>(e + 0x20);
        if (!x.ccd || !x.hash || !mem::readCString(mem::readOr<uintptr_t>(x.ccd + Asset_Name), x.name)) continue;
        splitEchoName(x.name, x.style, x.colour);
        found.push_back(x);
    }
    return !found.empty();
}

bool scanForCatalogue(std::vector<Echo>& found) {
    const uintptr_t vt = rva(Vtbl_PamServerCustomizeProjectionCharacterEntity);
    std::vector<uint8_t> buf(4 << 20);
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t addr = 0x10000; found.empty() && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == sizeof(mbi);
         addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || mbi.Protect != PAGE_READWRITE) continue;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        for (size_t off = 0; found.empty() && off < mbi.RegionSize; off += buf.size()) {
            const size_t n = std::min(buf.size(), static_cast<size_t>(mbi.RegionSize - off));
            if (!mem::read(base + off, buf.data(), n)) continue;
            const auto* q = reinterpret_cast<const uintptr_t*>(buf.data());
            for (size_t i = 0; found.empty() && i < n / 8; ++i)
                if (q[i] == vt) readCatalogue(base + off + i * 8, found);
        }
    }
    return !found.empty();
}
} // namespace

void onCustomizeEntity(void* entity) {
    if (!entity) return;
    std::lock_guard<std::mutex> lock(g_seenM);
    g_seen.insert(g_seen.begin(), reinterpret_cast<uintptr_t>(entity));
    if (g_seen.size() > 32) g_seen.pop_back();
}

// PamServerCustomizeProjectionCharacterEntity (time-trial markers; vtable 0x141c7ea80): +0x28 its data,
// data +0x20 PamEchoCustomizationDefinitionsMeta, meta +0x18 Array<PamEchoCustomizationData> (0x28
// each: +0x18 CustomizeCharacterData*, +0x20 u32 CustomizationHash). (See Ghidra: ..._vf4 0x143a83e70.)
bool findEchoes(bool allowScan) {
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (!g_echoes->empty()) return true;
    }
    std::vector<uintptr_t> seen;
    {
        std::lock_guard<std::mutex> lock(g_seenM);
        seen = g_seen;
    }
    std::vector<Echo> found;
    const DWORD start = GetTickCount();
    const char* how = "from a time-trial marker";
    for (uintptr_t ent : seen)
        if (readCatalogue(ent, found)) break;
    if (found.empty() && allowScan) {
        how = "by memory scan";
        scanForCatalogue(found);
    }
    if (found.empty()) {
        if (allowScan) MLOG("model: no ghost look list found yet (%.1f s)", (GetTickCount() - start) / 1000.0);
        return false;
    }
    std::sort(found.begin(), found.end(), [](const Echo& a, const Echo& b) {
        return a.style != b.style ? a.style < b.style : a.colour < b.colour;
    });
    MLOG("model: %llu ghost looks found %s (%.1f s), e.g. %s = %08x", static_cast<unsigned long long>(found.size()), how,
         (GetTickCount() - start) / 1000.0, found.front().name.c_str(), found.front().hash);
    std::lock_guard<std::mutex> lock(g_m);
    g_echoes = std::make_shared<const std::vector<Echo>>(std::move(found));
    return true;
}

} // namespace mecmp::game::models
