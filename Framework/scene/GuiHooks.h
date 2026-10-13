#pragma once
// GuiHooks - game UI drawn by native components.
//
// A native component has DrawDebug() for the editor's inspector, but nothing for the
// game's own screen: a HUD, a prompt, a speedometer. This is that seam. A component adds a
// callback in Start() and removes it when it goes away; SceneManager::OnGUI() - the game UI
// pass, drawn whether or not DevUI is up - calls every one of them, in the ImGui frame.
//
//     int hook_ = st::GuiHooks::Get().Add([this] { DrawHud(); });
//     ...
//     st::GuiHooks::Get().Remove(hook_);

#include <functional>
#include <map>

namespace st {

class GuiHooks {
public:
    static GuiHooks& Get ();

    int  Add (std::function<void()> draw);
    void Remove (int id);
    void Draw ();

private:
    std::map<int, std::function<void()>> hooks_;
    int next_ = 1;
};

} // namespace st
