#include "scene/DayNight.h"

#include "imgui.h"
#include "wiMath.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace st {

namespace {

constexpr float kPi    = 3.14159265358979323846f;
constexpr float kTwoPi = kPi * 2.0f;

float Radians (float degrees) { return degrees * (kPi / 180.0f); }
float Degrees (float radians) { return radians * (180.0f / kPi); }

float Saturate (float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float Wrap24 (float hours) {
    hours = std::fmod(hours, 24.0f);
    return hours < 0.0f ? hours + 24.0f : hours;
}

// 0 below `low`, 1 above `high`, smooth in between. Used for dusk, where a linear ramp
// leaves a visible corner at the horizon.
float SmoothStep (float low, float high, float x) {
    if (high <= low) return x >= high ? 1.0f : 0.0f;
    const float t = Saturate((x - low) / (high - low));
    return t * t * (3.0f - 2.0f * t);
}

XMFLOAT3 Lerp (const XMFLOAT3& a, const XMFLOAT3& b, float t) {
    return XMFLOAT3(a.x + (b.x - a.x) * t,
                    a.y + (b.y - a.y) * t,
                    a.z + (b.z - a.z) * t);
}

// A direction TOWARDS the sun, from an elevation above the horizon and an azimuth
// measured clockwise from north. World axes are the engine's: +Y up, +Z north, +X east.
XMFLOAT3 DirectionFrom (float elevation, float azimuth) {
    const float ce = std::cos(elevation);
    return XMFLOAT3(ce * std::sin(azimuth), std::sin(elevation), ce * std::cos(azimuth));
}

} // namespace

DayNight& DayNight::Get () {
    static DayNight instance;
    return instance;
}

// solar position

SolarPosition DayNight::ComputeSun (const Settings& s, float timeOfDay) {
    SolarPosition out;
    const float hours = Wrap24(timeOfDay);

    if (s.geographic) {
        // NOAA's solar position equations, in the usual order: fractional year, then the
        // equation of time and the declination as its Fourier series, then the hour angle.
        const float dayIndex = float(std::max(1, std::min(366, s.dayOfYear)) - 1);
        const float g = kTwoPi / 365.0f * (dayIndex + (hours - 12.0f) / 24.0f);

        // Minutes by which true solar noon differs from clock noon on this date.
        const float eqTime = 229.18f * (0.000075f
                           + 0.001868f * std::cos(g)      - 0.032077f * std::sin(g)
                           - 0.014615f * std::cos(2 * g)  - 0.040849f * std::sin(2 * g));

        // Declination: how far north or south of the equator the sun stands today.
        const float decl = 0.006918f
                         - 0.399912f * std::cos(g)     + 0.070257f * std::sin(g)
                         - 0.006758f * std::cos(2 * g) + 0.000907f * std::sin(2 * g)
                         - 0.002697f * std::cos(3 * g) + 0.001480f * std::sin(3 * g);

        // Clock time -> true solar time. 4 minutes per degree of longitude, minus the
        // whole hours the time zone already accounts for.
        const float offsetMinutes = eqTime + 4.0f * s.longitudeDegrees - 60.0f * s.timezoneHours;
        const float solarMinutes  = hours * 60.0f + offsetMinutes;
        const float hourAngle     = Radians(solarMinutes / 4.0f - 180.0f);

        const float lat = Radians(s.latitudeDegrees);
        const float sinElevation = std::sin(lat) * std::sin(decl) +
                                   std::cos(lat) * std::cos(decl) * std::cos(hourAngle);
        out.elevation = std::asin(std::max(-1.0f, std::min(1.0f, sinElevation)));

        // Azimuth from north, clockwise. The denominator collapses at the pole and at the
        // zenith, where azimuth is genuinely undefined - due south is the useful answer.
        const float denominator = std::cos(lat) * std::cos(out.elevation);
        float azimuth = kPi;
        if (std::abs(denominator) > 1e-5f) {
            const float cosAzimuth = (std::sin(decl) - std::sin(lat) * std::sin(out.elevation)) / denominator;
            azimuth = std::acos(std::max(-1.0f, std::min(1.0f, cosAzimuth)));
        }
        // Before local solar noon the sun is in the east, after it in the west.
        if (std::sin(hourAngle) > 0.0f)
            azimuth = kTwoPi - azimuth;

        out.azimuth = azimuth + s.northYaw;
    } else {
        // The simple arc: one full turn a day about a fixed heading, latitude ignored.
        // Noon puts the sun straight overhead, which no real place ever does - but it is
        // what a hand-rolled day/night loop looks like, and it is predictable to tune.
        const float theta = (hours - 12.0f) / 24.0f * kTwoPi;
        out.elevation = std::asin(std::max(-1.0f, std::min(1.0f, std::cos(theta))));
        out.azimuth   = s.sunYaw + s.northYaw + (std::sin(theta) < 0.0f ? kPi : 0.0f);
    }

    out.direction = DirectionFrom(out.elevation, out.azimuth);

    // daylight is the plain cosine term the lighting wants; twilight keeps the sky alive
    // for a while after the sun has set.
    out.daylight = Saturate(out.direction.y);
    out.twilight = SmoothStep(-Radians(s.twilightDegrees), Radians(10.0f), out.elevation);
    return out;
}

bool DayNight::SunTimes (float& sunriseHours, float& sunsetHours) const {
    // Sampled rather than solved: the simple arc and the geographic model do not share a
    // closed form, and 288 evaluations of a handful of trig calls is nothing for something
    // only the UI asks for.
    const int steps = 288;               // every 5 minutes
    float previous = ComputeSun(settings, 0.0f).elevation;
    bool  foundRise = false, foundSet = false;

    for (int i = 1; i <= steps; ++i) {
        const float hour = 24.0f * float(i) / float(steps);
        const float current = ComputeSun(settings, hour).elevation;

        if ((previous < 0.0f) != (current < 0.0f)) {
            // Bisect the five-minute bracket down to a couple of seconds.
            float lo = 24.0f * float(i - 1) / float(steps), hi = hour;
            for (int b = 0; b < 10; ++b) {
                const float mid = (lo + hi) * 0.5f;
                if ((ComputeSun(settings, mid).elevation < 0.0f) == (previous < 0.0f))
                    lo = mid;
                else
                    hi = mid;
            }
            const float crossing = (lo + hi) * 0.5f;
            if (current > previous) { sunriseHours = crossing; foundRise = true; }
            else                    { sunsetHours  = crossing; foundSet  = true; }
        }
        previous = current;
    }
    return foundRise && foundSet;
}

// binding

void DayNight::Attach (wi::ecs::Entity sun, wi::ecs::Entity weather) {
    sun_     = sun;
    weather_ = weather;
    owned_   = false;
}

bool DayNight::Adopt (wi::scene::Scene& scene) {
    // Something is already driving - a scene that built its own sun in Load() keeps it.
    if (sun_ != wi::ecs::INVALID_ENTITY && scene.lights.Contains(sun_))
        return true;

    wi::ecs::Entity found = wi::ecs::INVALID_ENTITY;
    for (size_t i = 0; i < scene.lights.GetCount(); ++i) {
        if (scene.lights[i].type == wi::scene::LightComponent::DIRECTIONAL) {
            found = scene.lights.GetEntity(i);
            break;
        }
    }
    if (found == wi::ecs::INVALID_ENTITY)
        return false;

    const wi::ecs::Entity weather = scene.weathers.GetCount() > 0
                                  ? scene.weathers.GetEntity(0)
                                  : wi::ecs::INVALID_ENTITY;
    Attach(found, weather);
    return true;
}

wi::ecs::Entity DayNight::Create (wi::scene::Scene& scene, const std::string& name) {
    if (Adopt(scene))
        return sun_;

    sun_ = scene.Entity_CreateLight(name, XMFLOAT3(0, 0, 0), XMFLOAT3(1, 1, 1),
                                    settings.sunIntensity, 0.0f,
                                    wi::scene::LightComponent::DIRECTIONAL);
    if (wi::scene::LightComponent* light = scene.lights.GetComponent(sun_)) {
        light->range = 1000.0f;
        light->SetCastShadow(true);
        light->cascade_distances = { 20.0f, 60.0f, 150.0f };
    }

    if (scene.weathers.GetCount() > 0) {
        weather_ = scene.weathers.GetEntity(0);
    } else {
        weather_ = wi::ecs::CreateEntity();
        scene.names.Create(weather_).name = "Weather";
        wi::scene::WeatherComponent& weather = scene.weathers.Create(weather_);
        weather.SetRealisticSky(true);
        weather.SetRealisticSkyAerialPerspective(true);
        weather.skyExposure = settings.skyExposure;
    }

    owned_ = true;
    return sun_;
}

void DayNight::Detach () {
    sun_     = wi::ecs::INVALID_ENTITY;
    weather_ = wi::ecs::INVALID_ENTITY;
    owned_   = false;
}

void DayNight::Destroy (wi::scene::Scene& scene) {
    if (owned_) {
        if (sun_ != wi::ecs::INVALID_ENTITY)     scene.Entity_Remove(sun_);
        if (weather_ != wi::ecs::INVALID_ENTITY) scene.Entity_Remove(weather_);
    }
    Detach();
}

// per frame

void DayNight::SetTime (float hours) {
    settings.timeOfDay = Wrap24(hours);
}

void DayNight::Update (wi::scene::Scene& scene, float dt) {
    if (!settings.enabled)
        return;

    if (settings.running && dt > 0.0f) {
        const float advanced = settings.timeOfDay + settings.hoursPerSecond * dt;
        if (settings.advanceDate && advanced >= 24.0f) {
            settings.dayOfYear += int(advanced / 24.0f);
            while (settings.dayOfYear > 365) settings.dayOfYear -= 365;
        }
        settings.timeOfDay = Wrap24(advanced);
    }

    sun_position_ = ComputeSun(settings, settings.timeOfDay);

    if (sun_ == wi::ecs::INVALID_ENTITY)
        return;

    // A directional light shines along its entity's local +Y (see RunLightUpdateSystem),
    // so the rotation that aims it is pitch = 90 degrees - elevation about X, then the
    // azimuth about Y. Writing the transform rather than LightComponent::direction is what
    // makes it survive the scene update, which recomputes direction from the world matrix.
    if (wi::scene::TransformComponent* transform = scene.transforms.GetComponent(sun_)) {
        const XMVECTOR rotation = XMQuaternionRotationRollPitchYaw(
            kPi * 0.5f - sun_position_.elevation, sun_position_.azimuth, 0.0f);
        XMStoreFloat4(&transform->rotation_local, rotation);
        transform->SetDirty();
        transform->UpdateTransform();
    }

    if (wi::scene::LightComponent* light = scene.lights.GetComponent(sun_)) {
        // Warm on the horizon, white overhead. Squaring biases the blend towards warm, so
        // sunrise and sunset last long enough to be worth looking at.
        const float k = sun_position_.daylight * sun_position_.daylight;
        light->color     = Lerp(settings.horizonColor, settings.dayColor, k);
        light->intensity = settings.sunIntensity * sun_position_.daylight;
    }

    if (settings.driveWeather && weather_ != wi::ecs::INVALID_ENTITY) {
        if (wi::scene::WeatherComponent* weather = scene.weathers.GetComponent(weather_)) {
            // Ambient follows twilight, not daylight: the sky is still lit for a while
            // after the sun itself has gone, and a hard cut to night reads as a bug.
            const float ambient = settings.ambientNight +
                                  (settings.ambientDay - settings.ambientNight) * sun_position_.twilight;
            weather->ambient = XMFLOAT3(ambient, ambient, ambient);
            weather->stars = settings.starsNight +
                             (settings.starsDay - settings.starsNight) * sun_position_.twilight;
            weather->skyExposure = settings.skyExposure;
        }
    }
}

// UI

std::string DayNight::ClockText (float hours) {
    const float wrapped = Wrap24(hours);
    const int   hh = int(wrapped);
    const int   mm = int((wrapped - float(hh)) * 60.0f) % 60;
    char text[8];
    std::snprintf(text, sizeof(text), "%02d:%02d", hh, mm);
    return text;
}

std::string DayNight::ClockText () const { return ClockText(settings.timeOfDay); }

void DayNight::GUI () {
    ImGui::Text("Time  %s", ClockText().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled(HasSun() ? "(sun attached)" : "(no sun)");

    ImGui::SliderFloat("Time of day", &settings.timeOfDay, 0.0f, 24.0f, "%.2f h");
    ImGui::Checkbox("Run clock", &settings.running);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderFloat("h / s", &settings.hoursPerSecond, 0.0f, 6.0f, "%.2f");

    if (ImGui::Button("Dawn"))     SetTime(6.0f);
    ImGui::SameLine();
    if (ImGui::Button("Noon"))     SetTime(12.0f);
    ImGui::SameLine();
    if (ImGui::Button("Sunset"))   SetTime(18.0f);
    ImGui::SameLine();
    if (ImGui::Button("Midnight")) SetTime(0.0f);

    float sunrise = 0.0f, sunset = 0.0f;
    if (SunTimes(sunrise, sunset)) {
        ImGui::TextDisabled("Sunrise %s   Sunset %s   Elevation %+.1f deg",
                            ClockText(sunrise).c_str(), ClockText(sunset).c_str(),
                            Degrees(sun_position_.elevation));
    } else {
        // A real answer above the polar circles, not a failure.
        ImGui::TextDisabled("The sun does not cross the horizon on day %d at this latitude.",
                            settings.dayOfYear);
    }
}

void DayNight::DevGUI (bool* open) {
    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Day / Night", open)) {
        ImGui::End();
        return;
    }

    ImGui::Checkbox("Enabled", &settings.enabled);
    ImGui::SameLine();
    ImGui::TextDisabled("off leaves the light exactly as the scene left it");

    ImGui::SeparatorText("Clock");
    GUI();
    ImGui::SliderInt("Day of year", &settings.dayOfYear, 1, 365);
    ImGui::SameLine();
    ImGui::Checkbox("Roll over", &settings.advanceDate);

    ImGui::SeparatorText("Place");
    ImGui::Checkbox("Geographic sun path", &settings.geographic);
    ImGui::BeginDisabled(!settings.geographic);
    ImGui::SliderFloat("Latitude",  &settings.latitudeDegrees,  -90.0f,  90.0f, "%.2f deg");
    ImGui::SliderFloat("Longitude", &settings.longitudeDegrees, -180.0f, 180.0f, "%.2f deg");
    ImGui::SliderFloat("Time zone", &settings.timezoneHours,    -12.0f,  14.0f, "UTC%+.1f");
    ImGui::EndDisabled();
    ImGui::BeginDisabled(settings.geographic);
    ImGui::SliderAngle("Sun heading", &settings.sunYaw, -180.0f, 180.0f);
    ImGui::EndDisabled();
    ImGui::SliderAngle("North is at", &settings.northYaw, -180.0f, 180.0f);

    ImGui::SeparatorText("Sun");
    ImGui::SliderFloat("Intensity", &settings.sunIntensity, 0.0f, 40.0f, "%.1f lux");
    ImGui::ColorEdit3("Day colour", &settings.dayColor.x);
    ImGui::ColorEdit3("Horizon colour", &settings.horizonColor.x);
    ImGui::SliderFloat("Twilight", &settings.twilightDegrees, 0.0f, 24.0f, "%.1f deg below");

    ImGui::SeparatorText("Sky");
    ImGui::Checkbox("Drive the weather", &settings.driveWeather);
    ImGui::BeginDisabled(!settings.driveWeather);
    ImGui::SliderFloat("Ambient night", &settings.ambientNight, 0.0f, 1.0f, "%.3f");
    ImGui::SliderFloat("Ambient day",   &settings.ambientDay,   0.0f, 1.0f, "%.3f");
    ImGui::SliderFloat("Stars night",   &settings.starsNight,   0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Stars day",     &settings.starsDay,     0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Sky exposure",  &settings.skyExposure,  0.0f, 4.0f, "%.2f");
    ImGui::EndDisabled();

    ImGui::SeparatorText("Target");
    if (HasSun()) {
        ImGui::Text("Sun entity     %llu%s", (unsigned long long)sun_,
                    owned_ ? "  (created here)" : "  (adopted)");
        ImGui::Text("Weather entity %llu", (unsigned long long)weather_);
        ImGui::Text("Direction      %.3f %.3f %.3f",
                    sun_position_.direction.x, sun_position_.direction.y, sun_position_.direction.z);
        ImGui::Text("Azimuth        %.1f deg from north", Degrees(sun_position_.azimuth));
        if (ImGui::Button("Detach")) Detach();
    } else {
        ImGui::TextDisabled("Nothing attached. AppConfig::dayNight decides whether a scene\n"
                            "load binds one; Off means the game does it itself.");
        if (ImGui::Button("Adopt the scene's sun")) Adopt(wi::scene::GetScene());
        ImGui::SameLine();
        if (ImGui::Button("Create one")) Create(wi::scene::GetScene());
    }

    ImGui::End();
}

// persistence

void DayNight::SaveTo (st::nbt::Tag& out) const {
    const Settings& s = settings;
    out.putBool ("enabled", s.enabled);
    out.putFloat("timeOfDay", s.timeOfDay);
    out.putBool ("running", s.running);
    out.putFloat("hoursPerSecond", s.hoursPerSecond);
    out.putInt  ("dayOfYear", s.dayOfYear);
    out.putBool ("advanceDate", s.advanceDate);
    out.putBool ("geographic", s.geographic);
    out.putFloat("latitude", s.latitudeDegrees);
    out.putFloat("longitude", s.longitudeDegrees);
    out.putFloat("timezone", s.timezoneHours);
    out.putFloat("northYaw", s.northYaw);
    out.putFloat("sunYaw", s.sunYaw);
    out.putFloat("sunIntensity", s.sunIntensity);
    out.putFloat("dayColorR", s.dayColor.x);
    out.putFloat("dayColorG", s.dayColor.y);
    out.putFloat("dayColorB", s.dayColor.z);
    out.putFloat("horizonColorR", s.horizonColor.x);
    out.putFloat("horizonColorG", s.horizonColor.y);
    out.putFloat("horizonColorB", s.horizonColor.z);
    out.putFloat("twilightDegrees", s.twilightDegrees);
    out.putBool ("driveWeather", s.driveWeather);
    out.putFloat("ambientNight", s.ambientNight);
    out.putFloat("ambientDay", s.ambientDay);
    out.putFloat("starsNight", s.starsNight);
    out.putFloat("starsDay", s.starsDay);
    out.putFloat("skyExposure", s.skyExposure);
}

void DayNight::LoadFrom (const st::nbt::Tag& in) {
    Settings s; // start from the struct defaults, so a missing key keeps its default
    s.enabled          = in.getBool ("enabled", s.enabled);
    s.timeOfDay        = in.getFloat("timeOfDay", s.timeOfDay);
    s.running          = in.getBool ("running", s.running);
    s.hoursPerSecond   = in.getFloat("hoursPerSecond", s.hoursPerSecond);
    s.dayOfYear        = in.getInt  ("dayOfYear", s.dayOfYear);
    s.advanceDate      = in.getBool ("advanceDate", s.advanceDate);
    s.geographic       = in.getBool ("geographic", s.geographic);
    s.latitudeDegrees  = in.getFloat("latitude", s.latitudeDegrees);
    s.longitudeDegrees = in.getFloat("longitude", s.longitudeDegrees);
    s.timezoneHours    = in.getFloat("timezone", s.timezoneHours);
    s.northYaw         = in.getFloat("northYaw", s.northYaw);
    s.sunYaw           = in.getFloat("sunYaw", s.sunYaw);
    s.sunIntensity     = in.getFloat("sunIntensity", s.sunIntensity);
    s.dayColor.x       = in.getFloat("dayColorR", s.dayColor.x);
    s.dayColor.y       = in.getFloat("dayColorG", s.dayColor.y);
    s.dayColor.z       = in.getFloat("dayColorB", s.dayColor.z);
    s.horizonColor.x   = in.getFloat("horizonColorR", s.horizonColor.x);
    s.horizonColor.y   = in.getFloat("horizonColorG", s.horizonColor.y);
    s.horizonColor.z   = in.getFloat("horizonColorB", s.horizonColor.z);
    s.twilightDegrees  = in.getFloat("twilightDegrees", s.twilightDegrees);
    s.driveWeather     = in.getBool ("driveWeather", s.driveWeather);
    s.ambientNight     = in.getFloat("ambientNight", s.ambientNight);
    s.ambientDay       = in.getFloat("ambientDay", s.ambientDay);
    s.starsNight       = in.getFloat("starsNight", s.starsNight);
    s.starsDay         = in.getFloat("starsDay", s.starsDay);
    s.skyExposure      = in.getFloat("skyExposure", s.skyExposure);
    settings = s;
}

} // namespace st
