#pragma once

class Scene {
public:
    virtual ~Scene() = default;
    virtual void Load() {}
    virtual void Update(float dt) {}
    virtual void OnGUI() {}
    virtual void Unload() {}
};
