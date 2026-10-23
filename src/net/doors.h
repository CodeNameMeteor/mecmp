#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string_view>
#include <vector>

namespace mecmp::net::doors {

constexpr uint16_t kPacketType = 19;
constexpr size_t kPayloadSize = 80, kMaxDoors = 8192, kMaxPeerDoors = 1024, kMaxReports = 128;
constexpr unsigned kSendBudget = 4;
constexpr uint32_t kSendWindowMs = 100, kRetryMs = 250, kRetryWindowMs = 10000, kInterestMs = 1000,
                   kPeerTimeoutMs = 5000;
constexpr float kEnterRadius = 200.f, kLeaveRadius = 250.f;

struct Key {
    uint64_t world = 0;
    std::array<int32_t, 12> placement{};
    bool operator==(const Key& o) const { return world == o.world && placement == o.placement; }
    bool operator<(const Key& o) const { return world != o.world ? world < o.world : placement < o.placement; }
};
uint64_t worldId(std::string_view name);
bool makeKey(uint64_t world, const float transform[16], Key& out);
bool validKey(const Key& key);
std::array<float, 3> position(const Key& key);

enum class Op : uint8_t { Interest, Open, State, Ack };
struct Message {
    Op op = Op::Interest, ack = Op::Interest;
    bool enabled = false, rejected = false;
    uint32_t id = 0, revision = 0;
    Key key;
    std::array<float, 3> position{};
};
size_t encode(const Message& message, char* payload, size_t capacity);
bool decode(const char* payload, size_t size, Message& out);
struct Delivery {
    uint32_t peer;
    Message message;
};

class Authority {
  public:
    void receive(uint32_t peer, const Message& message, uint32_t now);
    std::vector<Delivery> tick(uint32_t now);
    void remove(uint32_t peer) { peers_.erase(peer); }
    void reset();
    size_t size() const { return doors_.size(); }
    size_t peers() const { return peers_.size(); }

  private:
    struct Pending {
        Message message;
        uint32_t first = 0, last = 0;
        bool sent = false, exhausted = false;
    };
    struct Peer {
        Message interest;
        uint32_t heard = 0, budgetAt = 0;
        unsigned budget = 0;
        bool haveBudget = false;
        std::deque<Message> control;
        std::map<Key, Pending> watched;
        // Open-only revisions are record ordinals 1..kMaxDoors for this session.
        std::array<uint64_t, (kMaxDoors + 63) / 64> delivered{};
    };
    std::map<Key, uint32_t> doors_;
    std::map<uint32_t, Peer> peers_;
    uint32_t nextId_ = 0, nextRevision_ = 0;
    void refresh(Peer& peer);
};

class Client {
  public:
    void interest(uint64_t world, const float pos[3], bool enabled, uint32_t now);
    bool opened(const Key& key, uint32_t now);
    void receive(const Message& message, uint32_t now);
    std::vector<Message> tick(uint32_t now);
    bool isOpen(const Key& key) const { return interest_.enabled && states_.find(key) != states_.end(); }
    bool available(uint32_t now) const { return interest_.enabled && haveReply_ && now - lastReply_ <= kPeerTimeoutMs; }
    uint64_t failedReports() const { return failed_; }
    void reset();

  private:
    struct Report {
        Message message;
        uint32_t created = 0, last = 0;
        bool sent = false;
    };
    Message interest_;
    std::map<Key, uint32_t> states_;
    std::map<Key, Report> reports_;
    std::deque<Message> acknowledgments_;
    uint32_t nextId_ = 0, lastInterest_ = 0, lastReply_ = 0, budgetAt_ = 0;
    uint64_t failed_ = 0;
    unsigned budget_ = 0;
    bool interestDue_ = false, haveInterest_ = false, haveReply_ = false, haveBudget_ = false;
};

} // namespace mecmp::net::doors
