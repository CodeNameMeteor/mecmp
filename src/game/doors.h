#pragma once
#include <string>

namespace mecmp::game::doors {
bool install();
void tick();     // Server game thread; owns physics queries.
void maintain(); // Mod thread; bounded bootstrap and bank checks.
void levelChanged();
bool canOpen(); // Menu/render thread, cached selection only.
void requestOpen();
std::string status();
} // namespace mecmp::game::doors
