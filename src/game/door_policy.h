#pragma once
#include <cmath>

namespace mecmp::game::door_policy {
struct Observation {
    bool seen=false,closed=false,suppress=false,publishApplied=false;
    void reset() { *this={}; }
    void applied(bool publish) { suppress=true; publishApplied=publish; }
    bool observe(bool value,bool enabled) {
        const bool opened=seen&&closed&&!value;
        seen=true; closed=value;
        if (!opened) return false;
        const bool send=enabled&&(!suppress||publishApplied);
        suppress=false; publishApplied=false;
        return send;
    }
};

// Conservative stock-door bounds about the initial placement. The first physics
// hit must lie on its panel plane, rather than merely being close to its pivot.
inline bool matchesHit(const float mat[16],const float eye[3],const float hit[3],const float normal[3]) {
    float local[3]={},range=0,alignment=0;
    for (int i=0;i<3;++i) {
        if (!std::isfinite(eye[i])||!std::isfinite(hit[i])||!std::isfinite(normal[i])) return false;
        const float d=hit[i]-eye[i]; range+=d*d; alignment+=normal[i]*mat[8+i];
        for (int j=0;j<3;++j) local[j]+=(hit[i]-mat[12+i])*mat[4*j+i];
    }
    return range<=9.f&&range>=.01f&&std::fabs(alignment)>.9f&&
           std::fabs(local[0])<=1.1f&&local[1]>=-.15f&&local[1]<=2.7f&&std::fabs(local[2])<=.12f;
}
} // namespace mecmp::game::door_policy
