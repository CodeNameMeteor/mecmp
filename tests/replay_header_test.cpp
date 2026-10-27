// Tests for game/replay_header.h:
//   g++ -std=c++17 -Isrc tests/replay_header_test.cpp -o replay_header_test && ./replay_header_test
#include <cstdio>

#include "game/replay_header.h"

using namespace mecmp::game::replay_header;

static int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

int main() {
    // version 5, a hash, 23 bones, then keyframes: the stream is read 32 bits at a time, big-endian.
    const uint8_t game[] = {0x00, 0x00, 0x00, 0x05, 0x12, 0x34, 0x56, 0x78, 0x00, 0x00, 0x00, 0x17, 0xAA};
    const uint8_t mecmp[] = {0x00, 0x00, 0x00, 0x05, 0x9A, 0xBC, 0xDE, 0xF0, 0x00, 0x00, 0x00, 0x55};
    const uint8_t other[] = {0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55};
    const uint8_t littleEndian[] = {0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0x00, 0x00, 0x00};
    Header h;

    CHECK(parse(game, sizeof(game), h) && h.version == 5 && h.bones == 23);
    CHECK(!tooManyBones(h)); // the game's own runs are played

    CHECK(parse(mecmp, sizeof(mecmp), h) && h.version == 5 && h.bones == 85);
    CHECK(tooManyBones(h)); // an earlier beta's full-body run is refused

    CHECK(parse(other, sizeof(other), h) && !tooManyBones(h)); // another version: the game decides
    CHECK(parse(littleEndian, sizeof(littleEndian), h) && h.version != 5 && !tooManyBones(h));

    CHECK(!parse(game, 11, h));    // too short for a header
    CHECK(!parse(nullptr, 12, h));

    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("replay header: all checks passed\n");
    return 0;
}
