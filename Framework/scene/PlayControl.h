#pragma once
// st::PlayControl - the scene's transport: play, pause, step, reset.
//
// One thing decides how much time the SCENE sees each frame, and it is not the frame
// timer. st::App::Update hands the real frame delta to this class and passes what comes
// back to the scene and to the daylight system; everything that must keep working while
// the world is frozen - the DevUI, the editor camera, the input layer, the loading
// screen - keeps using the real one.
//
//   Playing            the world runs at wall-clock speed x timeScale
//   PlayingNoPhysics   everything animates, but nothing is simulated: no rigid bodies,
//                      no ragdolls, no cloth, no vehicles. This is the mode for looking
//                      at a scene's animation and logic without a stack of crates
//                      settling, drifting or exploding underneath it
//   Paused             the scene sees dt = 0 and physics is off. Nothing advances by
//                      itself, and Step() lets exactly one frame through at a time
//
// Reset() reloads the current scene from scratch and starts it playing - "from the
// start" in the only sense that survives a scene made of native components holding their
// own state, which is a real Unload() + Load() rather than an attempt to rewind.
//
// Physics is a GLOBAL engine switch (wi::physics::SetSimulationEnabled), so this only
// writes it when its own answer changes. A game that turns physics off for its own
// reasons keeps that setting until the transport state actually moves.
//
//     st::PlayControl& play = st::PlayControl::Get();
//     play.Pause();
//     play.Step();                       // one frame, physics included
//     play.SetState(st::PlayState::PlayingNoPhysics);
//     play.Reset();
//
// A scene that drives a camera from its own Update() should move it on
// PlayControl::Get().RealDelta() rather than on the dt it is handed, or the view freezes
// with the world and a paused scene cannot be looked at.

#include <cstdint>
#include <functional>

namespace st {

enum class PlayState {
    Playing,           // realtime
    PlayingNoPhysics,  // realtime, simulation off
    Paused,            // frozen
};

const char* ToString (PlayState state);

class PlayControl {
public:
    static PlayControl& Get ();

    // state

    PlayState State () const { return state_; }
    void      SetState (PlayState state);

    void Play ()               { SetState(PlayState::Playing); }
    void PlayWithoutPhysics () { SetState(PlayState::PlayingNoPhysics); }
    void Pause ()              { SetState(PlayState::Paused); }
    // Pause <-> whichever running mode was last used, so toggling out of a pause does
    // not silently turn physics back on for a scene that was deliberately running
    // without it.
    void TogglePause ();

    bool IsPaused  () const { return state_ == PlayState::Paused; }
    bool IsPlaying () const { return state_ != PlayState::Paused; }

    // Reload the current scene and start it playing. Deferred like any scene load: the
    // reload happens inside the next SceneManager::Update, not under the caller's feet.
    void Reset ();

    // Let `frames` frames through while paused, physics included. Stepping is the reason
    // Paused is dt = 0 rather than "the update is skipped": a stepped frame has to run
    // the same code path a played one does, or stepping shows something the game never
    // does.
    void Step (int frames = 1);
    bool Stepping () const { return stepsLeft_ > 0; }

    // Slow motion and fast forward, applied while playing. Not applied to a step, which
    // is always one honest frame.
    float timeScale = 1.0f;

    // The fixed delta a stepped frame is given. Real frame time is the wrong answer for
    // a step: the frame that renders the paused scene may have taken 300 ms, and a step
    // that advances the world by a third of a second is not a step.
    float stepDelta = 1.0f / 60.0f;

    // per frame

    // Called once by st::App::Update with the real frame delta. Returns the delta the
    // scene should be updated with, and applies the physics switch for this frame.
    float Apply (float realDelta);

    // The real frame delta, for anything that must keep moving while the scene does not.
    float RealDelta () const { return realDelta_; }
    // The delta the scene was given this frame.
    float SceneDelta () const { return sceneDelta_; }

    // Scene time since the last Reset() - what a scene should show as its own clock,
    // because it does not count paused frames.
    double   SceneTime  () const { return sceneTime_; }
    uint64_t SceneFrames () const { return sceneFrames_; }

    // st::App installs the reload; PlayControl has no other way to reach the
    // SceneManager, and should not know that it exists.
    void SetResetHook (std::function<void()> hook) { resetHook_ = std::move(hook); }

    // UI

    // The transport row: the buttons, the mode, the scene clock. `compact` draws it with
    // SmallButtons and a shorter readout, which is what fits inside a menu-bar row - the
    // editor toolbar draws it that way, beside the gizmo buttons.
    void GUI (bool compact = false);

private:
    PlayControl () = default;

    PlayState state_    = PlayState::Playing;
    PlayState lastPlay_ = PlayState::Playing;   // what TogglePause() returns to

    int      stepsLeft_   = 0;
    float    realDelta_   = 0.0f;
    float    sceneDelta_  = 0.0f;
    double   sceneTime_   = 0.0;
    uint64_t sceneFrames_ = 0;

    // What this class last told the physics system, so an unrelated call to
    // wi::physics::SetSimulationEnabled is not fought over every frame.
    bool physicsWanted_ = true;
    bool physicsWritten_ = false;

    std::function<void()> resetHook_;
};

} // namespace st
