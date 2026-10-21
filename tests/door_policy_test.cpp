#include "game/door_policy.h"
#include <cstdio>
#include <limits>
using namespace mecmp::game::door_policy;
static int failures=0;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n",__LINE__,#x); ++failures; } } while(0)
int main() {
    const float mat[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    const float eye[3]={0,1.6f,2},hit[3]={0,1.6f,.05f},normal[3]={0,0,1};
    CHECK(matchesHit(mat,eye,hit,normal));
    float wall[3]={0,1.6f,1}; CHECK(!matchesHit(mat,eye,wall,normal));
    float far[3]={0,1.6f,4}; CHECK(!matchesHit(mat,far,hit,normal));
    float side[3]={2,1.6f,0}; CHECK(!matchesHit(mat,eye,side,normal));
    float bad[3]={std::numeric_limits<float>::quiet_NaN(),0,0}; CHECK(!matchesHit(mat,eye,bad,normal));
    Observation state;
    CHECK(!state.observe(false,true)); // Never publish pre-existing open state.
    CHECK(!state.observe(true,true)); CHECK(state.observe(false,true));
    state.reset(); CHECK(!state.observe(true,true)); state.applied(false);
    CHECK(!state.observe(false,true)); // Remote open is not a new local event.
    state.reset(); CHECK(!state.observe(true,false)); CHECK(!state.observe(false,false));
    CHECK(!state.observe(false,true)); // Turning sync on must not export old local changes.
    state.reset(); CHECK(!state.observe(true,true)); state.applied(true); CHECK(state.observe(false,true));
    std::printf("door policy tests: %d failure(s)\n",failures); return failures?1:0;
}
