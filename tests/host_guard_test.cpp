// Tests for the host's per-player send limit (net/host_guard.h). Linux:
//   g++ -std=c++17 -Isrc tests/host_guard_test.cpp -o host_guard_test && ./host_guard_test
#include <cstdint>
#include <cstdio>

#include "net/host_guard.h"

using namespace mecmp::net::guard;

static int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

static void testBurstThenRefill() {
    Bucket b;
    b.at = 1000;
    // A burst of kBurst packets at one instant passes, the next one does not.
    int passed = 0;
    for (int i = 0; i < 300; ++i) passed += b.take(1000);
    CHECK(passed == static_cast<int>(kBurst));
    // One second later, kRatePerSec more pass.
    passed = 0;
    for (int i = 0; i < 300; ++i) passed += b.take(2000);
    CHECK(passed == static_cast<int>(kRatePerSec));
}

static void testSteadyRateIsAllowed() {
    // The normal traffic of a player (30 states, 1 hello a second) never hits the limit.
    Bucket b;
    b.at = 0;
    int dropped = 0;
    for (uint32_t t = 0; t < 60000; t += 33) dropped += !b.take(t);
    CHECK(dropped == 0);
}

static void testFloodIsCut() {
    // 1000 packets in one second: roughly the burst plus one second's rate pass, the rest do not.
    Bucket b;
    b.at = 0;
    int passed = 0;
    for (uint32_t i = 0; i < 1000; ++i) passed += b.take(i);
    CHECK(passed <= static_cast<int>(kBurst + kRatePerSec + 1));
    CHECK(passed >= static_cast<int>(kBurst));
}

static void testClockWrap() {
    // The millisecond clock wraps after 49.7 days: a bucket keeps refilling across the wrap.
    Bucket b;
    b.at = 0xFFFFFF00u;
    for (int i = 0; i < static_cast<int>(kBurst); ++i) b.take(0xFFFFFF00u);
    CHECK(!b.take(0xFFFFFF00u));
    CHECK(b.take(0xFFFFFF00u + 1000u)); // 1 s later, across the wrap: tokens are back
}

int main() {
    testBurstThenRefill();
    testSteadyRateIsAllowed();
    testFloodIsCut();
    testClockWrap();
    if (g_failures == 0) std::printf("host guard tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
