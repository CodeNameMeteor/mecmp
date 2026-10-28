#include "game/replay_check.h"

#include <mutex>

#include "core/log.h"
#include "game/engine.h"
#include "game/replay_header.h"

namespace mecmp::game::replay_check {
namespace {
std::mutex g_m;
std::vector<std::string> g_notices; // under g_m
} // namespace

uint8_t* beforeReplayDownloaded(void* message) {
    const auto m = reinterpret_cast<uintptr_t>(message);
    if (!m || mem::readOr<uint32_t>(m + ReplayMsg_Type) != kReplayDownloadedType) return nullptr;
    auto* ok = reinterpret_cast<uint8_t*>(m + ReplayMsg_Ok);
    const uintptr_t data = mem::readOr<uintptr_t>(m + ReplayMsg_Data);
    const uint32_t size = mem::readOr<uint32_t>(m + ReplayMsg_Size);
    uint8_t head[12] = {};
    replay_header::Header h;
    if (!mem::readOr<uint8_t>(m + ReplayMsg_Ok) || !data || size < sizeof(head) || !mem::read(data, head, sizeof(head)) ||
        !replay_header::parse(head, sizeof(head), h) || !replay_header::tooManyBones(h))
        return nullptr;
    MLOG("replay: a downloaded time-trial ghost has %u bones (the game plays %u; recorded with an older MECMP beta): "
         "not loaded, it would T-pose and crash the game",
         h.bones, replay_header::kGameBones);
    poke<uint8_t>(m + ReplayMsg_Ok, 0); // the handler's own "download failed" path: no ghost
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (g_notices.size() < 8)
            g_notices.push_back("This time trial's ghost was recorded with an older MECMP beta and can't be shown. "
                                "A new time replaces it.");
    }
    return ok;
}

void restore(uint8_t* okFlag) {
    if (okFlag) poke<uint8_t>(reinterpret_cast<uintptr_t>(okFlag), 1);
}

std::vector<std::string> takeNotices() {
    std::lock_guard<std::mutex> lock(g_m);
    std::vector<std::string> out;
    out.swap(g_notices);
    return out;
}

} // namespace mecmp::game::replay_check
