#pragma once
// DayNight - the framework's daylight / time-of-day system.
//
// It owns one clock and drives one directional light plus the scene's weather from it:
// the sun's direction, its colour and intensity, the ambient level, the stars and the sky
// exposure. Every game had been writing that loop itself; this is the one copy.
//
// The sun is placed ASTRONOMICALLY by default - latitude, longitude, time zone and day of
// year through the NOAA solar position equations - so the sun rises in the east, tracks
// the right arc for the place and season, and the day is the right length. Turn
// `geographic` off for the simpler "sun on a hoop overhead" arc, which is what hand-rolled
// day/night loops usually are, and which ignores latitude entirely.
//
// Reaching it:
//
//     st::DayNight& day = st::DayNight::Get();
//     day.settings.timeOfDay = 6.5f;      // 06:30
//     day.settings.running    = true;      // let the clock run
//
// st::App calls Update() once a frame, BEFORE the scene update, so the light system that
// copies the sun into the weather sees this frame's direction rather than last frame's.
//
// Which light it drives is `AppConfig::dayNight`:
//   Off    - nothing, until something calls Adopt()/Create()/Attach() itself (the default)
//   Adopt  - on every scene load, take over the scene's first directional light
//   Create - the same, but make a sun and a weather if the scene has neither
//
// Off is the default on purpose: a scene that aims its own sun, or a map whose sun was
// placed by hand in the editor, must not have it silently re-aimed on load.

#include "wiScene.h"
#include "io/Nbt.h"

#include <string>

namespace st {

// How st::App binds the system to a scene when one is loaded.
enum class DayNightMode {
    Off,     // never bind - the game (or a "stDayNight" component) does it itself
    Adopt,   // take over the scene's first directional light, if it has one
    Create,  // adopt, and make a sun + weather when the scene has none
};

// Where the sun is right now.
struct SolarPosition {
    float    elevation = 0.0f;                  // radians above the horizon; negative at night
    float    azimuth   = 0.0f;                  // radians clockwise from north, world space
    XMFLOAT3 direction = XMFLOAT3(0, 1, 0);     // unit vector TOWARDS the sun; matches LightComponent::direction
    float    daylight  = 0.0f;                  // 0 below the horizon .. 1 at the zenith
    float    twilight  = 0.0f;                  // 0 fully dark .. 1 full day, with a soft dusk
};

class DayNight {
public:
    struct Settings {
        bool enabled = true;          // off leaves the light alone without detaching

        // clock
        float timeOfDay      = 12.0f; // hours [0, 24)
        bool  running        = true;  // advance the clock with the frame
        float hoursPerSecond = 0.25f; // game hours per real second (0.25 = a day in 96 s)
        int   dayOfYear      = 172;   // 1..365; 172 is around the June solstice
        bool  advanceDate    = true;  // roll dayOfYear over at midnight

        // place. Only used when `geographic` is on.
        bool  geographic       = true;
        float latitudeDegrees  = 47.5f;  // + north
        float longitudeDegrees = 19.0f;  // + east
        float timezoneHours    = 1.0f;   // offset from UTC that timeOfDay is stated in
        // Which way compass north points in world space, as a rotation about +Y. The
        // engine has no opinion about it, so a map built facing "north" along +X sets
        // this rather than moving the map.
        float northYaw = 0.0f;

        // Simple mode only: the fixed heading the sun rises and sets along.
        float sunYaw = -0.6f;

        // look
        float    sunIntensity = 10.0f;                        // lux at the zenith
        XMFLOAT3 dayColor     = XMFLOAT3(1.00f, 0.98f, 0.92f); // high sun
        XMFLOAT3 horizonColor = XMFLOAT3(1.00f, 0.55f, 0.30f); // sun on the horizon

        // How far below the horizon the sky is still lit. 6 degrees is civil twilight;
        // raising it lengthens dusk and dawn.
        float twilightDegrees = 8.0f;

        // weather. Off leaves ambient/stars/exposure to whatever the map set.
        bool  driveWeather = true;
        float ambientNight = 0.03f;
        float ambientDay   = 0.40f;
        float starsNight   = 0.60f;
        float starsDay     = 0.00f;
        float skyExposure  = 1.00f;
    };
    Settings settings;

    static DayNight& Get();

    // binding

    // Drive `sun` (a directional light) and `weather`. Either may be INVALID_ENTITY;
    // a missing weather just means nothing but the light is driven.
    void Attach (wi::ecs::Entity sun, wi::ecs::Entity weather = wi::ecs::INVALID_ENTITY);

    // Take over the scene's first directional light and first weather. Returns false when
    // the scene has no directional light. Does nothing if something is already attached,
    // so a scene that called Create() in its Load() keeps what it made.
    bool Adopt (wi::scene::Scene& scene);

    // Adopt, or build a sun (shadow-casting directional light) and a realistic-sky weather
    // when the scene has none. Returns the sun entity.
    wi::ecs::Entity Create (wi::scene::Scene& scene, const std::string& name = "SunLight");

    // Stop driving anything. The entities stay in the scene.
    void Detach ();

    // Detach, and remove the entities if this system is what created them.
    void Destroy (wi::scene::Scene& scene);

    bool            HasSun        () const { return sun_ != wi::ecs::INVALID_ENTITY; }
    bool            OwnsSun       () const { return owned_; }
    wi::ecs::Entity SunEntity     () const { return sun_; }
    wi::ecs::Entity WeatherEntity () const { return weather_; }

    // per frame

    // Advance the clock and write the sun. st::App calls this before the scene update.
    void Update (wi::scene::Scene& scene, float dt);

    // clock

    void  SetTime (float hours);                  // wraps into [0, 24)
    float Time    () const { return settings.timeOfDay; }
    // "06:42" for the current time; ClockText(h) for any hour value.
    std::string ClockText () const;
    static std::string ClockText (float hours);

    // Where the sun was at the last Update().
    const SolarPosition& Sun () const { return sun_position_; }

    // Sunrise / sunset for the current day and place, in hours. False when the sun does
    // not cross the horizon at all that day - a polar summer or winter, which is a real
    // answer at a high enough latitude and not an error.
    bool SunTimes (float& sunriseHours, float& sunsetHours) const;

    // UI

    // Player-facing / scene GUI: the clock, the speed, the place. Drop it into a game's
    // own options panel or a scene's OnGUI.
    void GUI ();
    // The dockable DevUI window, with everything including the look settings.
    void DevGUI (bool* open);

    // persistence - the "daynight" child of options.stad
    void SaveTo  (st::nbt::Tag& out) const;
    void LoadFrom (const st::nbt::Tag& in);

    // Pure solar math, no scene and no state: where the sun is for these settings at this
    // hour. Public because a game may want tomorrow's sunrise without moving the clock.
    static SolarPosition ComputeSun (const Settings& settings, float timeOfDay);

private:
    DayNight() = default;

    wi::ecs::Entity sun_     = wi::ecs::INVALID_ENTITY;
    wi::ecs::Entity weather_ = wi::ecs::INVALID_ENTITY;
    bool            owned_   = false;   // Create() made them, so Destroy() may remove them

    SolarPosition sun_position_;
};

} // namespace st
