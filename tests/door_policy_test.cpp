#include "game/door_policy.h"
#include <cstdio>
#include <limits>
using namespace mecmp::game::door_policy;
static int failures = 0;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            std::printf("FAILED line %d: %s\n", __LINE__, #x);                                                         \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)
int main() {
    Lifecycle life;
    const auto before = life.ticket;
    life.beginDestroy();
    CHECK(!life.accepts(before));
    const auto during = life.ticket;
    CHECK(!life.accepts(during));
    life.endDestroy();
    CHECK(!life.accepts(during));
    CHECK(life.accepts(life.ticket));
    const float mat[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float eye[3] = {0, 1.6f, 2}, hit[3] = {0, 1.6f, .05f}, normal[3] = {0, 0, 1};
    CHECK(matchesHit(mat, eye, hit, normal));
    float wall[3] = {0, 1.6f, 1};
    CHECK(!matchesHit(mat, eye, wall, normal));
    float far[3] = {0, 1.6f, 4};
    CHECK(!matchesHit(mat, far, hit, normal));
    float side[3] = {2, 1.6f, 0};
    CHECK(!matchesHit(mat, eye, side, normal));
    float bad[3] = {std::numeric_limits<float>::quiet_NaN(), 0, 0};
    CHECK(!matchesHit(mat, eye, bad, normal));
    CHECK(publishPulse(true, false, true, false));
    CHECK(!publishPulse(false, false, true, false));
    CHECK(!publishPulse(true, true, true, false));
    CHECK(!publishPulse(true, false, false, false));
    CHECK(!publishPulse(true, false, true, true));
    CHECK(allowOpening(false));
    CHECK(!allowOpening(true));
    Application apply;
    CHECK(apply.due(0));
    apply.attempted(0);
    CHECK(!apply.due(2999));
    CHECK(apply.due(3000));
    apply.attempted(3000);
    CHECK(!apply.due(5999));
    CHECK(apply.due(6000));
    apply.attempted(6000);
    CHECK(!apply.due(9000));
    CHECK(!apply.due(9999999));
    CHECK(apply.exhausted());
    Application success;
    success.attempted(10);
    success.opened();
    CHECK(!success.due(9999999));
    CHECK(!success.exhausted());
    CHECK(!bootstrapNeeded(0, 0));
    CHECK(!bootstrapNeeded(0, 1));
    CHECK(bootstrapNeeded(10, 10));
    CHECK(!bootstrapNeeded(10, 11));
    std::printf("door policy tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
