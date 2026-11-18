// Tests for game/data_registry.h (key decoding and the table plausibility check):
//   g++ -std=c++17 -Isrc tests/data_registry_test.cpp -o data_registry_test && ./data_registry_test
#include <cstdio>

#include "game/data_registry.h"

using namespace mecmp::game::registry;

static int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

int main() {
    // key = partition << 40 | classId << 16 | index (FUN_143119500: (partition * 0x1000000 + classId) * 0x10000 + index).
    const uint64_t key = ((uint64_t{0x12} * 0x1000000 + 0x0345) * 0x10000) + 0x0007;
    CHECK(partitionOf(key) == 0x12);
    CHECK(classIdOf(key) == 0x0345);
    CHECK(indexOf(key) == 0x0007);
    CHECK(key == (uint64_t{0x12} << 40 | uint64_t{0x345} << 16 | 7));
    CHECK(partitionOf(0xffffffffffffffffull) == 0xffff && classIdOf(0xffffffffffffffffull) == 0xffff);

    // The real dump's table: begin 0x14e7a1110, end 0x14e7a9b10, capacity 0x14e7a9d70 = 2208 entries.
    CHECK(plausibleTable(0x14e7a1110, 0x14e7a9b10, 0x14e7a9d70));
    CHECK((0x14e7a9b10ull - 0x14e7a1110ull) / 16 == 2208);
    // An empty table (begin == end) is fine; the loader's initial state (all null) is not a table.
    CHECK(plausibleTable(0x1000, 0x1000, 0x1100));
    CHECK(!plausibleTable(0, 0, 0));
    // Reversed, over capacity, misaligned, or an implausible size are refused.
    CHECK(!plausibleTable(0x2000, 0x1000, 0x3000));
    CHECK(!plausibleTable(0x1000, 0x2000, 0x1800));
    CHECK(!plausibleTable(0x1004, 0x2004, 0x3004));
    CHECK(!plausibleTable(0x1000, 0x1008, 0x1010));
    CHECK(!plausibleTable(0x1000, 0x1000 + 16 * 100, 0x1000 + 16 * 100, 50));
    // Class-id range (the echo catalogue meta: 0x071e..0x071e, read from the image).
    const uint64_t meta = (uint64_t{0x30} << 40) | (uint64_t{0x071e} << 16) | 2;
    CHECK(inClassRange(meta, 0x071e, 0x071e));
    CHECK(!inClassRange(meta, 0x071f, 0x0720));
    CHECK(inClassRange((uint64_t{1} << 40) | (uint64_t{0x0720} << 16), 0x071e, 0x0725)); // sub-class
    CHECK(!inClassRange(meta, 0, 0xffff));                                              // range not read
    // FUN_143119500 skips partitions 0x7d5..0xbbc (1000 ids).
    CHECK(!partitionSkipped(0x7d4, 0x7d5, 1000) && partitionSkipped(0x7d5, 0x7d5, 1000));
    CHECK(partitionSkipped(0xbbc, 0x7d5, 1000) && !partitionSkipped(0xbbd, 0x7d5, 1000));
    CHECK(!partitionSkipped(3, 0x7d5, 1000));
    if (g_failures == 0) std::printf("data_registry_test: all passed\n");
    return g_failures ? 1 : 0;
}
