#include "game/player_audio.h"
#include "game/engine.h"
#include "game/level.h"
#include "game/ghosts.h"
#include "game/native_vocals.h"
#include "core/config.h"
#include "core/log.h"
#include "net/net.h"
#include <array>
#include <atomic>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <algorithm>
#include <cstring>
#include <memory>

namespace mecmp::game::player_audio {
namespace {
// PC build: SoundEntity::handleEvent, SoundSystem::createSound, and native
// Sound instance virtuals. Verified against the live game's vtables and the
// SoundEntity handler/update disassembly; see docs/PLAYER_AUDIO.md.
constexpr uintptr_t kSoundEntityVtable = 0x141ad0338, kSoundEvent = 0x14301a170;
constexpr uintptr_t kSoundSystem = 0x1423dd580, kSoundSystemVtable = 0x141abd460;
constexpr uintptr_t kInstanceVtable = 0x141acfad0;
constexpr uintptr_t kSoundEntityDataType = 0x1427c7c80;
constexpr uintptr_t kNativePropertyValue = 0x142a424b0, kFloatType = 0x1427b0cb8;
constexpr uintptr_t kVocalEntityVtable = 0x141b2e078, kVocalEvent = 0x1432bc760;
constexpr uintptr_t kVocalDataType = 0x142810e60, kSoundUpdater = 0x1423e2f20;
using EventFn = void(*)(uintptr_t, const void*);
EventFn g_original{}, g_originalVocal{};
std::atomic<bool> g_installed{false};
std::atomic<bool> g_vocalsInstalled{false};
std::mutex g_mutex;
struct Source {
    uintptr_t data = 0, asset = 0, entity = 0;
    net::PlayerAudio previous;
    DWORD sentAt = 0;
    DWORD durationMs = 2000;
};
std::array<Source, net::kPlayerAudioSourceCount> g_sources;
std::map<uintptr_t, uint8_t> g_localData;
uint16_t g_voiceRealm = 0;
bool g_haveVoiceRealm = false;
struct Captured { net::PlayerAudio sample; DWORD at; };
std::deque<Captured> g_captured;
std::atomic<uint64_t> g_captures{0}, g_played{0};
std::atomic<uint64_t> g_vocalCaptures{0}, g_vocalPlayed{0};

uint32_t timestamp() {
    static const double frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart); }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return static_cast<uint32_t>(static_cast<uint64_t>(static_cast<double>(c.QuadPart) * 1000. / frequency));
}
int sourceIndex(uintptr_t asset) {
    std::string name;
    if (!asset || !mem::readCString(mem::readOr<uintptr_t>(asset + Asset_Name), name, 192)) return -1;
    for (size_t i = 0; i < net::kPlayerSoundCount; ++i) if (name == net::kPlayerSounds[i]) return static_cast<int>(i);
    return -1;
}
// Only graph-declared inputs may be driven, even by a peer with a valid packet.
bool graphKey(uintptr_t asset, size_t listOffset, uint32_t key) {
    const auto graph = mem::readOr<uintptr_t>(asset + 0x38);
    const auto list = graph ? mem::readOr<uintptr_t>(graph + listOffset) : 0;
    const auto count = list ? mem::readOr<uint32_t>(list - 4) : 0;
    if (count > 256) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const auto entry = mem::readOr<uintptr_t>(list + i * 8);
        if (entry && mem::readOr<uint32_t>(entry + 0x14) == key) return true;
    }
    return false;
}
net::PlayerAudio snapshot(uintptr_t entity, uint8_t source, uint32_t event) {
    net::PlayerAudio sample;
    sample.source = source; sample.event = event; sample.timeMs = timestamp();
    uintptr_t begin = 0, end = 0;
    if (!mem::read(entity + 0x48, begin) || !mem::read(entity + 0x50, end) ||
        end < begin || (end - begin) % 0x28 || end - begin > 128 * 0x28) return sample;
    for (auto p = begin; p < end && sample.count < net::kMaxAudioParams; p += 0x28) {
        // Native SoundEntity's property table: kind 0 = scalar, 1 = transform,
        // 2 = SoundDataAsset. Transforms come from the peer's bounded pose;
        // pointers/assets are deliberately never sent.
        if (mem::readOr<uint32_t>(p + 0x24, ~0u) != 0) continue;
        const auto key = mem::readOr<uint32_t>(p + 0x20);
        if (!key) continue;
        const auto value = reinterpret_cast<uintptr_t(*)(uintptr_t, uintptr_t)>(rva(kNativePropertyValue))(p, rva(kFloatType));
        float scalar = 0;
        if (!value || !mem::read(value, scalar) || !std::isfinite(scalar) || std::fabs(scalar) > 100000.f) continue;
        sample.params[sample.count++] = {key, scalar};
    }
    return sample;
}
bool sameInputs(const net::PlayerAudio& a, const net::PlayerAudio& b) {
    if (a.count != b.count) return false;
    for (uint8_t i = 0; i < a.count; ++i)
        if (a.params[i].key != b.params[i].key || a.params[i].value != b.params[i].value) return false;
    return true;
}
// Called only for a sound owned by the local character. The native conversation
// submission needs the entity bus's realm, as VoiceOverConversationEntity does.
void rememberRealm(uintptr_t entity) {
    if (!g_haveVoiceRealm) {
        const auto bus = mem::readOr<uintptr_t>(entity + 0x20);
        if (bus) { g_voiceRealm = native_vocals::realmForBus(bus); g_haveVoiceRealm = true; }
    }
}
void eventHook(uintptr_t entity, const void* event) {
    // Leave local sound handling intact and capture native inputs at the event
    // edge, so a short foot contact cannot disappear between network updates.
    g_original(entity, event);
    const DWORD after = GetLastError();
    if (!net::running()) { SetLastError(after); return; }
    const auto data = mem::readOr<uintptr_t>(entity + 0x28);
    const auto asset = data ? mem::readOr<uintptr_t>(data + 0x18) : 0;
    const int index = sourceIndex(asset);
    uint32_t hash = 0;
    if (index >= 0 && event && mem::read(reinterpret_cast<uintptr_t>(event) + 8, hash) && graphKey(asset, 0x80, hash)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto& source = g_sources[static_cast<size_t>(index)];
        if (!g_localData.count(data) || source.asset != asset) { SetLastError(after); return; }
        rememberRealm(entity);
        auto sample = snapshot(entity, static_cast<uint8_t>(index), hash);
        source.data = data; source.asset = asset; source.entity = entity;
        source.previous = sample; source.sentAt = GetTickCount();
        if (g_captured.size() >= 64) g_captured.pop_front();
        g_captured.push_back({sample, GetTickCount()}); ++g_captures;
    }
    SetLastError(after);
}
void vocalEventHook(uintptr_t entity, const void* event) {
    const DWORD before = GetLastError();
    uint32_t hash = 0;
    if (event) mem::read(reinterpret_cast<uintptr_t>(event) + 8, hash);
    const auto flags = mem::readOr<uint32_t>(entity + 0x3c);
    SetLastError(before);
    g_originalVocal(entity, event);
    const DWORD after = GetLastError();
    if (net::running() && (hash == net::kVocalCancel || (hash == net::kVocalTrigger && !(flags & 2)))) {
        const auto data = mem::readOr<uintptr_t>(entity + 0x28);
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto found = g_localData.find(data);
        if (found != g_localData.end() && net::vocalSource(found->second)) {
            rememberRealm(entity);
            net::PlayerAudio sample;
            sample.source = found->second; sample.event = hash; sample.timeMs = timestamp();
            if (g_captured.size() >= 64) g_captured.pop_front();
            g_captured.push_back({sample, GetTickCount()}); ++g_captures; ++g_vocalCaptures;
        }
    }
    SetLastError(after);
}
struct Playing {
    uintptr_t instance = 0;
    DWORD heardAt = 0, lifetimeMs = 2000;
    std::unique_ptr<native_vocals::Playback> vocal;
};
std::map<uint32_t, std::array<Playing, net::kPlayerAudioSourceCount>> g_playing; // client thread only
std::vector<std::unique_ptr<native_vocals::Playback>> g_retiringVocals;
std::deque<net::RemoteAudio> g_pending;
uintptr_t g_world = 0, g_character = 0;
DWORD g_tickAt = 0, g_discoverAt = 0;
void release(Playing& sound) {
    if (sound.vocal) {
        native_vocals::cancel(*sound.vocal);
        g_retiringVocals.push_back(std::move(sound.vocal));
    }
    if (sound.instance && mem::readOr<uintptr_t>(sound.instance) == rva(kInstanceVtable)) {
        if (const auto stop = gameVirtual(reinterpret_cast<void*>(sound.instance), 0x40))
            reinterpret_cast<void(*)(uintptr_t, int)>(stop)(sound.instance, 1);
        if (const auto dispose = gameVirtual(reinterpret_cast<void*>(sound.instance), 0x48))
            reinterpret_cast<void(*)(uintptr_t)>(dispose)(sound.instance);
    }
    sound = {};
}
void retireVocals() {
    const auto now = GetTickCount();
    for (auto it = g_retiringVocals.begin(); it != g_retiringVocals.end();) {
        if (!native_vocals::referenced(**it)) {
            if (!(*it)->unreferencedAt) (*it)->unreferencedAt = now;
            if (now - (*it)->unreferencedAt >= 1000) { it = g_retiringVocals.erase(it); continue; }
        } else (*it)->unreferencedAt = 0;
        native_vocals::cancel(**it); ++it;
    }
}
void clearSounds() {
    for (auto& peer : g_playing) for (auto& sound : peer.second) release(sound);
    g_playing.clear();
    g_pending.clear();
}
void catalogue(uintptr_t character) {
    const auto data = mem::readOr<uintptr_t>(character + Character_Data);
    const auto list = data ? mem::readOr<uintptr_t>(data + 0x60) : 0;
    const auto count = list ? mem::readOr<uint32_t>(list - 4) : 0;
    if (count > 256) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto remember = [](uintptr_t component) {
        if (!component) return;
        if (isKindOf(reinterpret_cast<void*>(component), kSoundEntityDataType)) {
            const auto asset = mem::readOr<uintptr_t>(component + 0x18);
            const int index = sourceIndex(asset);
            if (index >= 0) {
                auto& s = g_sources[static_cast<size_t>(index)]; s.data = component; s.asset = asset;
                g_localData[component] = static_cast<uint8_t>(index);
            }
        } else if (isKindOf(reinterpret_cast<void*>(component), kVocalDataType)) {
            const auto tracks = mem::readOr<uintptr_t>(component + 0x20);
            const auto groups = mem::readOr<uintptr_t>(component + 0x18);
            if (!tracks || !groups || mem::readOr<uint32_t>(tracks - 4) != 1 || mem::readOr<uint32_t>(groups - 4) != 1) return;
            const auto track = mem::readOr<uintptr_t>(tracks);
            const auto clips = track ? mem::readOr<uintptr_t>(track + 0x58) : 0;
            if (!clips || mem::readOr<uint32_t>(clips - 4) != 1) return;
            const auto clip = mem::readOr<uintptr_t>(clips);
            const auto takes = clip ? mem::readOr<uintptr_t>(clip + 0x18) : 0;
            if (!takes || mem::readOr<uint32_t>(takes - 4) != 1) return;
            const auto wave = mem::readOr<uintptr_t>(takes);
            std::string name;
            if (!wave || !mem::readCString(mem::readOr<uintptr_t>(wave + Asset_Name), name)) return;
            for (size_t i = 0; i < net::kPlayerVocalCount; ++i) if (name == net::kPlayerVocalWaves[i]) {
                const auto index = static_cast<uint8_t>(net::kPlayerSoundCount + i);
                auto& s = g_sources[index]; s.data = component; s.asset = wave;
                const float duration = mem::readOr<float>(wave + 0x68, 2.f);
                s.durationMs = static_cast<DWORD>((std::isfinite(duration) ? std::clamp(duration, .1f, 15.f) : 2.f) * 1000.f) + 500;
                g_localData[component] = index;
                break;
            }
        }
    };
    std::set<uintptr_t> visited;
    const auto visit = [&](const auto& self, uintptr_t component, int depth) -> void {
        if (!component || depth > 3 || !visited.insert(component).second) return;
        remember(component);
        if (!isKindOf(reinterpret_cast<void*>(component), 0x1427e7a90)) return;
        const auto blueprint = mem::readOr<uintptr_t>(component + 0x18);
        std::string name;
        if (!blueprint || !mem::readCString(mem::readOr<uintptr_t>(blueprint + Asset_Name), name) ||
            (name != "Sound/Logic/Faith_Movement_Audio" && name != "Sound/Logic/Faith_Combat_Audio" &&
             name != "ME_Sound/Character/Faith_Emotes")) return;
        const auto objects = mem::readOr<uintptr_t>(blueprint + 0x40);
        const auto objectsCount = objects ? mem::readOr<uint32_t>(objects - 4) : 0;
        if (objectsCount > 256) return;
        for (uint32_t j = 0; j < objectsCount; ++j) self(self, mem::readOr<uintptr_t>(objects + j * 8), depth + 1);
    };
    for (uint32_t i = 0; i < count; ++i) visit(visit, mem::readOr<uintptr_t>(list + i * 8), 0);
    size_t graphs = 0, vocals = 0;
    for (size_t i = 0; i < g_sources.size(); ++i) if (g_sources[i].data) {
        if (i < net::kPlayerSoundCount) ++graphs; else ++vocals;
    }
    MLOG("audio: local catalogue has %zu sound graphs and %zu vocal clips", graphs, vocals);
}
void discoverActiveSounds() {
    // Looping breathing can start before our catalogue is ready. The native
    // updater lists active SoundEntity +0x30 subobjects; discover them without
    // waiting for a new Start event or scanning the game's heap.
    const auto updater = mem::readOr<uintptr_t>(rva(kSoundUpdater));
    const auto begin = updater ? mem::readOr<uintptr_t>(updater + 0x28) : 0;
    const auto end = updater ? mem::readOr<uintptr_t>(updater + 0x30) : 0;
    if (!begin || end < begin || (end - begin) % 8 || end - begin > 8192 * 8) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto p = begin; p < end; p += 8) {
        const auto subobject = mem::readOr<uintptr_t>(p);
        if (subobject < 0x10030) continue;
        const auto entity = subobject - 0x30;
        if (mem::readOr<uintptr_t>(entity) != rva(kSoundEntityVtable)) continue;
        const auto data = mem::readOr<uintptr_t>(entity + 0x28);
        const auto found = g_localData.find(data);
        if (found == g_localData.end() || found->second >= net::kPlayerSoundCount) continue;
        g_sources[found->second].entity = entity;
        g_sources[found->second].data = data;
        rememberRealm(entity);
    }
}
}
bool install() {
    if (g_installed) return true;
    const auto slot = rva(kSoundEntityVtable) + 0x20;
    if (mem::readOr<uintptr_t>(slot) != rva(kSoundEvent)) {
        MLOG("audio: SoundEntity handler differs; player sound hooks not installed"); return false;
    }
    g_original = reinterpret_cast<EventFn>(rva(kSoundEvent));
    if (!mem::patchPointer(slot, reinterpret_cast<uintptr_t>(&eventHook), nullptr)) return false;
    g_installed = true;
    const auto vocalSlot = rva(kVocalEntityVtable) + 0x20;
    if (mem::readOr<uintptr_t>(vocalSlot) == rva(kVocalEvent)) {
        g_originalVocal = reinterpret_cast<EventFn>(rva(kVocalEvent));
        g_vocalsInstalled = mem::patchPointer(vocalSlot, reinterpret_cast<uintptr_t>(&vocalEventHook), nullptr);
    }
    MLOG("audio: native movement/combat/breathing capture installed; effort vocals %s", g_vocalsInstalled ? "installed" : "unavailable");
    return true;
}
void tick() {
    if (!g_installed) return;
    const DWORD now = GetTickCount();
    if (now == g_tickAt) return; // two replay updater passes share this frame
    g_tickAt = now;
    retireVocals();
    const auto world = level::serverWorld();
    const auto character = world && !level::inFrontEnd() ? reinterpret_cast<uintptr_t>(reinterpret_cast<void*(*)()>(rva(Fn_LocalClientCharacter))()) : 0;
    if (world != g_world || character != g_character) {
        clearSounds();
        { std::lock_guard<std::mutex> lock(g_mutex); g_sources = {}; g_localData.clear(); g_captured.clear(); g_haveVoiceRealm = false; }
        g_world = world; g_character = character;
        g_discoverAt = now - 1000;
        if (character) catalogue(character);
    }
    const auto incoming = net::takeAudio();
    const auto settings = config::get();
    if (!world || !character || !net::running()) { clearSounds(); return; }
    if (now - g_discoverAt >= 1000) { discoverActiveSounds(); g_discoverAt = now; }
    std::deque<Captured> captured;
    std::array<Source, net::kPlayerAudioSourceCount> sources;
    uint16_t voiceRealm = 0;
    bool haveVoiceRealm = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex); captured.swap(g_captured); sources = g_sources;
        voiceRealm = g_voiceRealm; haveVoiceRealm = g_haveVoiceRealm;
    }
    for (const auto& c : captured) if (now - c.at < 500) net::publishAudio(c.sample);
    // Continuous inputs (speed, material) and a heartbeat for active graphs.
    for (size_t i = 0; i < net::kPlayerSoundCount; ++i) {
        auto& s = sources[i];
        if (!s.entity || now - s.sentAt < 200 || mem::readOr<uintptr_t>(s.entity) != rva(kSoundEntityVtable) ||
            mem::readOr<uintptr_t>(s.entity + 0x28) != s.data || !mem::readOr<uintptr_t>(s.entity + 0x88)) continue;
        auto sample = snapshot(s.entity, static_cast<uint8_t>(i), 0);
        if (sameInputs(sample, s.previous) && now - s.sentAt < 750) continue;
        net::publishAudio(sample);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_sources[i].previous = sample; g_sources[i].sentAt = now;
    }
    if (!settings->playerAudio) { clearSounds(); return; }
    net::PlayerState local;
    if (!net::localState(local)) { clearSounds(); return; }
    const auto nearby = [&](uint32_t peer, net::PlayerState& state) {
        uint32_t age = 0;
        if (!net::remoteState(peer, state, age) || age > 1000 || !(state.flags & net::kStateInLevel)) return false;
        double distance = 0;
        for (int i = 0; i < 3; ++i) distance += (state.pos[i] - local.pos[i]) * (state.pos[i] - local.pos[i]);
        return distance <= settings->playerAudioDistance * settings->playerAudioDistance;
    };
    const auto place = [](uintptr_t instance, const net::PlayerState& state) {
        alignas(16) const auto transform = transformFromYaw(state.pos, state.yaw);
        if (const auto fn = gameVirtual(reinterpret_cast<void*>(instance), 0x68))
            reinterpret_cast<void(*)(uintptr_t, const LinearTransform*)>(fn)(instance, &transform);
        // Native finite zero velocity prevents teleport Doppler spikes.
        alignas(16) const float zero[4] = {};
        if (const auto fn = gameVirtual(reinterpret_cast<void*>(instance), 0x78))
            reinterpret_cast<void(*)(uintptr_t, const float*)>(fn)(instance, zero);
    };
    for (auto it = g_playing.begin(); it != g_playing.end();) {
        net::PlayerState state;
        if (!nearby(it->first, state)) { for (auto& s : it->second) release(s); it = g_playing.erase(it); continue; }
        for (auto& s : it->second) {
            if (now - s.heardAt > s.lifetimeMs) release(s);
            else if (s.instance) place(s.instance, state);
        }
        ++it;
    }
    for (const auto& message : incoming) {
        if (g_pending.size() >= 256) g_pending.pop_front();
        g_pending.push_back(message);
    }
    std::vector<net::RemoteAudio> due;
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        if (now - it->receivedAt > 750) { it = g_pending.erase(it); continue; }
        if (static_cast<int32_t>(now - it->playAt) < 0) { ++it; continue; }
        due.push_back(*it); it = g_pending.erase(it);
    }
    for (const auto& message : due) {
        if (now - message.receivedAt > 500 || !net::validAudio(message.sample)) continue;
        net::PlayerState state;
        if (!nearby(message.peer, state)) continue;
        const auto& sample = message.sample;
        const auto& source = sources[sample.source];
        const bool vocal = net::vocalSource(sample.source);
        const auto asset = source.asset;
        if (!asset) continue;
        if (!vocal) {
            if (sourceIndex(asset) != sample.source || (sample.event && !graphKey(asset, 0x80, sample.event))) continue;
            bool valid = true;
            for (uint8_t i = 0; i < sample.count; ++i) valid &= graphKey(asset, 0x70, sample.params[i].key);
            if (!valid) continue;
        }
        auto found = g_playing.find(message.peer);
        if (found == g_playing.end()) {
            if (g_playing.size() >= 15 || (vocal && sample.event == net::kVocalCancel)) continue;
            found = g_playing.emplace(message.peer, std::array<Playing, net::kPlayerAudioSourceCount>{}).first;
        }
        auto& sound = found->second[sample.source];
        if (vocal) {
            if (sample.event == net::kVocalCancel) { release(sound); continue; }
            if (!g_vocalsInstalled || !haveVoiceRealm || g_retiringVocals.size() >= 128) continue;
            const auto actor = ghosts::clientCharacter(message.peer);
            if (!actor) continue;
            release(sound);
            auto playback = std::make_unique<native_vocals::Playback>();
            if (!native_vocals::start(*playback, source.data, actor, voiceRealm)) continue;
            sound.vocal = std::move(playback); sound.heardAt = now; sound.lifetimeMs = source.durationMs;
            ++g_played; ++g_vocalPlayed;
            continue;
        }
        if (!sound.instance) {
            const auto system = mem::readOr<uintptr_t>(rva(kSoundSystem));
            if (!system || mem::readOr<uintptr_t>(system) != rva(kSoundSystemVtable)) continue;
            const auto create = gameVirtual(reinterpret_cast<void*>(system), 0x40);
            if (!create) continue;
            sound.instance = reinterpret_cast<uintptr_t(*)(uintptr_t, uintptr_t)>(create)(system, asset);
            if (!sound.instance || mem::readOr<uintptr_t>(sound.instance) != rva(kInstanceVtable)) { sound.instance = 0; continue; }
            place(sound.instance, state);
            // Like SoundEntity::onInit, start the graph through its native event.
            reinterpret_cast<void(*)(uintptr_t, int)>(gameVirtual(reinterpret_cast<void*>(sound.instance), 0x40))(sound.instance, 0);
        }
        sound.heardAt = now;
        place(sound.instance, state);
        for (uint8_t i = 0; i < sample.count; ++i)
            reinterpret_cast<void(*)(uintptr_t, uint32_t, float)>(gameVirtual(reinterpret_cast<void*>(sound.instance), 0x18))(
                sound.instance, sample.params[i].key, sample.params[i].value);
        if (sample.event)
            reinterpret_cast<void(*)(uintptr_t, uint32_t)>(gameVirtual(reinterpret_cast<void*>(sound.instance), 0))(
                sound.instance, sample.event);
        ++g_played;
    }
}
std::string status() {
    if (!g_installed) return "Native player sound hook unavailable";
    return "Captured " + std::to_string(g_captures.load()) + ", played " + std::to_string(g_played.load()) +
           " (vocals " + std::to_string(g_vocalCaptures.load()) + "/" + std::to_string(g_vocalPlayed.load()) + ")" +
           (g_vocalsInstalled ? "" : " - vocal hook unavailable");
}
}
