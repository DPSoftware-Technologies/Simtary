#pragma once
// The in-game multiplayer window: host a game, join one by address, see who is connected, leave.
// Game UI, not DevUI - a game draws it from RenderUI() behind a key of its own:
//
//     if (ImGui::IsKeyPressed(ImGuiKey_F10, false)) open = !open;
//     st::net::DrawMultiplayerWindow(open, [this] { sceneManager.Reload(); });
//
// What it does is call NetSession (StartHost / OpenToLan / StartClient / Leave), so everything it
// can do the command line can too. The name, address, port and transport are remembered in the
// options store ("net.*"). Both sides must be in the same scene: the vehicles of a scene are the
// players' vehicles (Framework/net/VehicleSync).

#include <functional>

namespace st::net {

// `reloadScene` is called after leaving a network game: the session took the scene's vehicle template
// away, so reloading is what gives a single player their vehicle back.
void DrawMultiplayerWindow (bool& open, const std::function<void()>& reloadScene);

} // namespace st::net
