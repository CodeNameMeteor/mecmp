#pragma once
// Short notices at the top of the screen when a player joins or leaves (render thread, inside the
// ImGui frame). Players come and go as net::remotes() sees them; the solo-test mirror is left out.

namespace mecmp::ui::notices {

void draw();

} // namespace mecmp::ui::notices
