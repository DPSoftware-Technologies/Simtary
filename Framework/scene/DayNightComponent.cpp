// The "sticDayNight" native component: the daylight system, attached from the editor.
//
// Put it on the scene's directional light and the whole day/night cycle is scene data -
// place, date, clock speed and look are NCA_ parameters, they save with the map, and no
// game code is involved. That is the difference between this and AppConfig::dayNight:
// the config binds the system to whatever sun a scene happens to have, this says which
// sun, with which settings, from inside the scene itself.
//
//     NCI_0                  = "sticDayNight"
//     NCA_0_timeOfDay        = 7.5
//     NCA_0_hoursPerSecond   = 0.25
//     NCA_0_latitudeDegrees  = 47.5
//
// One instance drives the system; a second one on another entity simply wins when its
// Start() runs, because the system holds a single sun.

#include "scene/DayNight.h"
#include "stNativeComponent.h"

#include "imgui.h"

namespace st {

struct DayNightComponent : wi::scene::NativeComponent {
    // Mirrors DayNight::Settings. Kept as its own copy so the inspector edits, and
    // SaveBoundParams(), work on this entity's metadata rather than on global state.
    float timeOfDay        = 12.0f;
    bool  running          = true;
    float hoursPerSecond   = 0.25f;
    int   dayOfYear        = 172;
    bool  advanceDate      = true;

    bool  geographic       = true;
    float latitudeDegrees  = 47.5f;
    float longitudeDegrees = 19.0f;
    float timezoneHours    = 1.0f;
    float northYaw         = 0.0f;
    float sunYaw           = -0.6f;

    float sunIntensity     = 10.0f;
    float twilightDegrees  = 8.0f;

    bool  driveWeather     = true;
    float ambientNight     = 0.03f;
    float ambientDay       = 0.40f;
    float starsNight       = 0.60f;
    float starsDay         = 0.00f;
    float skyExposure      = 1.00f;

    void Start () override {
        Bind(timeOfDay, "timeOfDay");
        Bind(running, "running");
        Bind(hoursPerSecond, "hoursPerSecond");
        Bind(dayOfYear, "dayOfYear");
        Bind(advanceDate, "advanceDate");
        Bind(geographic, "geographic");
        Bind(latitudeDegrees, "latitudeDegrees");
        Bind(longitudeDegrees, "longitudeDegrees");
        Bind(timezoneHours, "timezoneHours");
        Bind(northYaw, "northYaw");
        Bind(sunYaw, "sunYaw");
        Bind(sunIntensity, "sunIntensity");
        Bind(twilightDegrees, "twilightDegrees");
        Bind(driveWeather, "driveWeather");
        Bind(ambientNight, "ambientNight");
        Bind(ambientDay, "ambientDay");
        Bind(starsNight, "starsNight");
        Bind(starsDay, "starsDay");
        Bind(skyExposure, "skyExposure");

        Apply();

        // Start is main-thread by contract, which is what makes binding the system here
        // safe. The weather is the scene's first one, the same one Adopt() would pick.
        const wi::ecs::Entity weather = (scene != nullptr && scene->weathers.GetCount() > 0)
                                      ? scene->weathers.GetEntity(0)
                                      : wi::ecs::INVALID_ENTITY;
        DayNight::Get().Attach(entity, weather);
    }

    void Destroy () override {
        // Only let go of the system if it is still pointed at this entity - another
        // instance may have taken it over in the meantime.
        if (DayNight::Get().SunEntity() == entity)
            DayNight::Get().Detach();
    }

    // Push this instance's parameters into the system. The system owns the clock once it
    // is running, so timeOfDay is handed over only here, not every frame.
    void Apply () {
        DayNight::Settings& s = DayNight::Get().settings;
        s.enabled          = true;
        s.timeOfDay        = timeOfDay;
        s.running          = running;
        s.hoursPerSecond   = hoursPerSecond;
        s.dayOfYear        = dayOfYear;
        s.advanceDate      = advanceDate;
        s.geographic       = geographic;
        s.latitudeDegrees  = latitudeDegrees;
        s.longitudeDegrees = longitudeDegrees;
        s.timezoneHours    = timezoneHours;
        s.northYaw         = northYaw;
        s.sunYaw           = sunYaw;
        s.sunIntensity     = sunIntensity;
        s.twilightDegrees  = twilightDegrees;
        s.driveWeather     = driveWeather;
        s.ambientNight     = ambientNight;
        s.ambientDay       = ambientDay;
        s.starsNight       = starsNight;
        s.starsDay         = starsDay;
        s.skyExposure      = skyExposure;
    }

    void DrawDebug () override {
        // Every widget feeds one flag, so an edit can be written back to the NCA_ metadata
        // Bind() read it from - without that, tuning the cycle here is lost on reload.
        bool dirty = false;

        ImGui::TextDisabled("Clock now: %s", DayNight::Get().ClockText().c_str());
        dirty |= ImGui::SliderFloat("Start time", &timeOfDay, 0.0f, 24.0f, "%.2f h");
        dirty |= ImGui::Checkbox("Run clock", &running);
        dirty |= ImGui::SliderFloat("Hours / second", &hoursPerSecond, 0.0f, 6.0f, "%.2f");
        dirty |= ImGui::SliderInt("Day of year", &dayOfYear, 1, 365);
        dirty |= ImGui::Checkbox("Roll the date over", &advanceDate);

        ImGui::SeparatorText("Place");
        dirty |= ImGui::Checkbox("Geographic sun path", &geographic);
        ImGui::BeginDisabled(!geographic);
        dirty |= ImGui::SliderFloat("Latitude",  &latitudeDegrees,  -90.0f,  90.0f, "%.2f deg");
        dirty |= ImGui::SliderFloat("Longitude", &longitudeDegrees, -180.0f, 180.0f, "%.2f deg");
        dirty |= ImGui::SliderFloat("Time zone", &timezoneHours,    -12.0f,  14.0f, "UTC%+.1f");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(geographic);
        dirty |= ImGui::SliderAngle("Sun heading", &sunYaw, -180.0f, 180.0f);
        ImGui::EndDisabled();
        dirty |= ImGui::SliderAngle("North is at", &northYaw, -180.0f, 180.0f);

        ImGui::SeparatorText("Look");
        dirty |= ImGui::SliderFloat("Sun intensity", &sunIntensity, 0.0f, 40.0f, "%.1f lux");
        dirty |= ImGui::SliderFloat("Twilight", &twilightDegrees, 0.0f, 24.0f, "%.1f deg below");
        dirty |= ImGui::Checkbox("Drive the weather", &driveWeather);
        ImGui::BeginDisabled(!driveWeather);
        dirty |= ImGui::SliderFloat("Ambient night", &ambientNight, 0.0f, 1.0f, "%.3f");
        dirty |= ImGui::SliderFloat("Ambient day",   &ambientDay,   0.0f, 1.0f, "%.3f");
        dirty |= ImGui::SliderFloat("Stars night",   &starsNight,   0.0f, 1.0f, "%.2f");
        dirty |= ImGui::SliderFloat("Stars day",     &starsDay,     0.0f, 1.0f, "%.2f");
        dirty |= ImGui::SliderFloat("Sky exposure",  &skyExposure,  0.0f, 4.0f, "%.2f");
        ImGui::EndDisabled();

        if (dirty) {
            Apply();
            SaveBoundParams();
        }

        ImGui::Separator();
        if (DayNight::Get().SunEntity() != entity)
            ImGui::TextDisabled("Another sticDayNight has taken the system over.");
        else
            ImGui::TextDisabled("Driving this entity's light. Full controls: Simtary > Day / Night.");
    }
};

} // namespace st

// The registration macro pastes the type name into an identifier, so it cannot take a
// qualified one. The alias is the type, so GetNativeTypeID<> still resolves to the same
// identity a GetComponent<st::DayNightComponent>() lookup asks for.
using StDayNightComponent = st::DayNightComponent;
ST_REGISTER_FRAMEWORK_COMPONENT_AS(StDayNightComponent, "sticDayNight", "Scene")
