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

// weather

const char* ToString (WeatherPreset preset) {
    switch (preset) {
        case WeatherPreset::Clear:    return "Clear";
        case WeatherPreset::Fair:     return "Fair";
        case WeatherPreset::Cloudy:   return "Cloudy";
        case WeatherPreset::Overcast: return "Overcast";
        case WeatherPreset::Rain:     return "Rain";
        case WeatherPreset::Storm:    return "Storm";
        case WeatherPreset::Fog:      return "Fog";
        default:                      return "Custom";
    }
}

WeatherState Blend (const WeatherState& a, const WeatherState& b, float t) {
    const float k = Saturate(t);
    WeatherState out;
    out.cloudiness     = a.cloudiness     + (b.cloudiness     - a.cloudiness)     * k;
    out.cloudDarkness  = a.cloudDarkness  + (b.cloudDarkness  - a.cloudDarkness)  * k;
    out.exposureScale  = a.exposureScale  + (b.exposureScale  - a.exposureScale)  * k;
    out.ambientScale   = a.ambientScale   + (b.ambientScale   - a.ambientScale)   * k;
    out.fogDensity     = a.fogDensity     + (b.fogDensity     - a.fogDensity)     * k;
    out.fogStart       = a.fogStart       + (b.fogStart       - a.fogStart)       * k;
    out.fogHeightStart = a.fogHeightStart + (b.fogHeightStart - a.fogHeightStart) * k;
    out.fogHeightEnd   = a.fogHeightEnd   + (b.fogHeightEnd   - a.fogHeightEnd)   * k;
    out.windSpeed      = a.windSpeed      + (b.windSpeed      - a.windSpeed)      * k;
    out.windTurbulence = a.windTurbulence + (b.windTurbulence - a.windTurbulence) * k;
    out.windWaveSize   = a.windWaveSize   + (b.windWaveSize   - a.windWaveSize)   * k;
    out.rain           = a.rain           + (b.rain           - a.rain)           * k;
    out.rainSpeed      = a.rainSpeed      + (b.rainSpeed      - a.rainSpeed)      * k;

    // The heading takes the short way round: blending 350 to 10 degrees the arithmetic
    // way swings the wind three quarters of the way round the compass on its way to a
    // 20 degree change, and every tree in the scene leans through the whole detour.
    float delta = std::fmod(b.windDegrees - a.windDegrees + 540.0f, 360.0f) - 180.0f;
    out.windDegrees = std::fmod(a.windDegrees + delta * k + 360.0f, 360.0f);
    return out;
}

WeatherState DayNight::Preset (WeatherPreset preset) {
    WeatherState w;
    switch (preset) {
        case WeatherPreset::Clear:
            w.cloudiness = 0.08f; w.cloudDarkness = 0.00f;
            w.exposureScale = 1.00f; w.ambientScale = 1.00f;
            w.fogDensity = 0.000f; w.fogStart = 200.0f; w.fogHeightEnd = 30.0f;
            w.windSpeed = 1.5f; w.windTurbulence = 3.0f; w.windWaveSize = 1.0f;
            break;
        case WeatherPreset::Fair:
            w.cloudiness = 0.35f; w.cloudDarkness = 0.05f;
            w.exposureScale = 1.00f; w.ambientScale = 1.00f;
            w.fogDensity = 0.002f; w.fogStart = 150.0f; w.fogHeightEnd = 40.0f;
            w.windSpeed = 3.0f; w.windTurbulence = 5.0f; w.windWaveSize = 1.0f;
            break;
        case WeatherPreset::Cloudy:
            w.cloudiness = 0.65f; w.cloudDarkness = 0.25f;
            w.exposureScale = 0.90f; w.ambientScale = 0.95f;
            w.fogDensity = 0.006f; w.fogStart = 120.0f; w.fogHeightEnd = 60.0f;
            w.windSpeed = 6.0f; w.windTurbulence = 6.0f; w.windWaveSize = 1.2f;
            break;
        case WeatherPreset::Overcast:
            w.cloudiness = 0.92f; w.cloudDarkness = 0.55f;
            w.exposureScale = 0.72f; w.ambientScale = 0.85f;
            w.fogDensity = 0.012f; w.fogStart = 90.0f; w.fogHeightEnd = 80.0f;
            w.windSpeed = 8.0f; w.windTurbulence = 7.0f; w.windWaveSize = 1.4f;
            break;
        case WeatherPreset::Rain:
            w.cloudiness = 0.97f; w.cloudDarkness = 0.70f;
            w.exposureScale = 0.60f; w.ambientScale = 0.80f;
            w.fogDensity = 0.030f; w.fogStart = 60.0f; w.fogHeightEnd = 90.0f;
            w.windSpeed = 11.0f; w.windTurbulence = 9.0f; w.windWaveSize = 1.6f;
            w.rain = 0.65f; w.rainSpeed = 1.2f;
            break;
        case WeatherPreset::Storm:
            w.cloudiness = 1.00f; w.cloudDarkness = 0.85f;
            w.exposureScale = 0.45f; w.ambientScale = 0.70f;
            w.fogDensity = 0.060f; w.fogStart = 40.0f; w.fogHeightEnd = 120.0f;
            w.windSpeed = 22.0f; w.windTurbulence = 14.0f; w.windWaveSize = 2.0f;
            w.rain = 1.00f; w.rainSpeed = 1.8f;
            break;
        case WeatherPreset::Fog:
            w.cloudiness = 0.50f; w.cloudDarkness = 0.35f;
            w.exposureScale = 0.75f; w.ambientScale = 0.90f;
            // Dense, close and low: the whole point of fog is that it hides the middle
            // distance, so it starts almost at the camera and ends below the rooftops.
            w.fogDensity = 0.250f; w.fogStart = 5.0f; w.fogHeightEnd = 25.0f;
            w.windSpeed = 0.5f; w.windTurbulence = 2.0f; w.windWaveSize = 0.8f;
            break;
        default:
            break;
    }
    return w;
}

void DayNight::SetWeather (const WeatherState& state, float blendSeconds) {
    const float seconds = blendSeconds < 0.0f ? settings.transitionSeconds : blendSeconds;

    // The blend starts from what is on screen NOW, not from the previous target. Change
    // your mind half way through a transition and the sky carries on from where it got
    // to, instead of jumping back to start the new one.
    weather_from_    = weather_now_;
    settings.weather = state;
    blendTotal_      = seconds;
    blend_           = seconds > 0.0f ? 0.0f : 1.0f;
    if (blend_ >= 1.0f) weather_now_ = state;
    preset_          = WeatherPreset::Count;
}

void DayNight::SetWeather (WeatherPreset preset, float blendSeconds) {
    SetWeather(Preset(preset), blendSeconds);
    preset_ = preset;   // set last: the state overload cannot know a name was involved
}

void DayNight::StepAutoWeather (float gameHoursAdvanced) {
    if (!settings.autoWeather || settings.autoIntervalHours <= 0.0f)
        return;

    autoHours_ += gameHoursAdvanced;
    if (autoHours_ < settings.autoIntervalHours)
        return;
    autoHours_ = 0.0f;

    // Which presets are allowed, as a list to draw from. An empty mask means the feature
    // is on but nothing may be picked, which is a configuration to leave alone rather
    // than to "fix" by picking anyway.
    WeatherPreset allowed[int(WeatherPreset::Count)];
    int count = 0;
    for (int i = 0; i < int(WeatherPreset::Count); ++i)
        if (settings.autoPresets & (1u << i)) allowed[count++] = WeatherPreset(i);
    if (count == 0)
        return;

    // A 64-bit LCG, seeded from settings, rather than std::rand: the same seed has to
    // produce the same weather on every machine for a replay or a shared session to
    // agree about whether it was raining.
    if (rng_ == 0u) rng_ = settings.autoSeed != 0u ? settings.autoSeed : 1u;
    rng_ = rng_ * 1664525u + 1013904223u;
    const int pick = int((rng_ >> 16) % uint32_t(count));

    // Never redraw the weather that is already showing - it would look like the system
    // had stopped. With one preset allowed that is exactly what is wanted, so it is
    // only skipped when there is somewhere else to go.
    WeatherPreset next = allowed[pick];
    if (next == preset_ && count > 1)
        next = allowed[(pick + 1) % count];

    SetWeather(next);
}

void DayNight::ApplyWeather (wi::scene::Scene& scene) {
    wi::scene::WeatherComponent* weather = scene.weathers.GetComponent(weather_);
    if (weather == nullptr)
        return;

    const WeatherState& w = weather_now_;

    weather->SetVolumetricClouds(settings.clouds);
    weather->SetVolumetricCloudsCastShadow(settings.clouds && settings.cloudShadows);
    weather->SetHeightFog(settings.heightFog);

    // Ambient follows twilight, not daylight: the sky is still lit for a while after the
    // sun itself has gone, and a hard cut to night reads as a bug. Overcast then scales
    // the whole thing down, because a lid of cloud is darker at noon and *lighter* at
    // midnight than a clear sky - which is why the scale is applied to the day end only.
    const float ambientBase = settings.ambientNight +
                              (settings.ambientDay - settings.ambientNight) * sun_position_.twilight;
    const float ambient = settings.ambientNight +
                          (ambientBase - settings.ambientNight) * w.ambientScale;
    weather->ambient = XMFLOAT3(ambient, ambient, ambient);

    // Cloud cover hides the stars. Without this a solid overcast at midnight is a grey
    // ceiling with a full starfield burning through it.
    const float stars = settings.starsNight +
                        (settings.starsDay - settings.starsNight) * sun_position_.twilight;
    weather->stars = Saturate(stars * (1.0f - 0.9f * Saturate(w.cloudiness)));

    weather->skyExposure = settings.skyExposure * w.exposureScale;

    // fog
    weather->fogStart       = w.fogStart;
    weather->fogDensity     = w.fogDensity;
    weather->fogHeightStart = w.fogHeightStart;
    weather->fogHeightEnd   = std::max(w.fogHeightStart + 0.1f, w.fogHeightEnd);

    // wind. The engine wants a direction VECTOR whose length is part of the force (see
    // wiScene.cpp: emitters use direction * speed, springs use its magnitude), so the
    // heading becomes a unit vector scaled by how hard it is blowing, and the speed
    // stays the scalar everything else reads.
    const float windRadians = Radians(w.windDegrees) + settings.northYaw;
    const float windForce   = Saturate(w.windSpeed / 20.0f);
    weather->windDirection  = XMFLOAT3(std::sin(windRadians) * windForce, 0.0f,
                                       std::cos(windRadians) * windForce);
    weather->windSpeed      = w.windSpeed;
    weather->windRandomness = w.windTurbulence;
    weather->windWaveSize   = w.windWaveSize;

    // rain
    weather->rain_amount = Saturate(w.rain);
    weather->rain_speed  = w.rainSpeed;

    // The clouds themselves. Coverage is two knobs in the engine: `coverageAmount`
    // scales the weather-map noise, `coverageMinimum` raises its floor. Amount alone
    // never closes the sky - there are always holes where the noise is low - so a solid
    // overcast needs the floor lifted, and only the top of the range does that.
    VolumetricCloudParameters& clouds = weather->volumetricCloudParameters;
    const float c = Saturate(w.cloudiness);
    clouds.layerFirst.coverageAmount  = 0.35f + 0.65f * c;
    clouds.layerFirst.coverageMinimum = c > 0.6f ? (c - 0.6f) / 0.4f * 0.5f : 0.0f;
    clouds.layerFirst.rainAmount      = Saturate(w.rain);

    // Grey by absorbing more light rather than by tinting: a darkened albedo is what a
    // thick cloud actually looks like, where a blue-grey tint reads as haze.
    const float albedo = 0.9f - 0.6f * Saturate(w.cloudDarkness);
    clouds.layerFirst.albedo = float3(albedo, albedo, albedo);

    // Clouds move with the weather, not on their own schedule.
    clouds.layerFirst.windAngle         = windRadians;
    clouds.layerFirst.windSpeed         = 10.0f + w.windSpeed * 3.0f;
    clouds.layerFirst.coverageWindAngle = windRadians;
    clouds.layerFirst.coverageWindSpeed = 20.0f + w.windSpeed * 4.0f;
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

    float gameHours = 0.0f;
    if (settings.running && dt > 0.0f) {
        gameHours = settings.hoursPerSecond * dt;
        const float advanced = settings.timeOfDay + gameHours;
        if (settings.advanceDate && advanced >= 24.0f) {
            settings.dayOfYear += int(advanced / 24.0f);
            while (settings.dayOfYear > 365) settings.dayOfYear -= 365;
        }
        settings.timeOfDay = Wrap24(advanced);
    }

    StepAutoWeather(gameHours);

    // The transition runs on REAL seconds, not game hours: "ten seconds to turn stormy"
    // has to mean ten seconds whether the clock is stopped or a day is going by every
    // minute. Eased, because a linear cloud coverage ramp has a visible start and stop.
    if (blend_ < 1.0f && blendTotal_ > 0.0f && dt > 0.0f)
        blend_ = Saturate(blend_ + dt / blendTotal_);
    weather_now_ = blend_ >= 1.0f
                 ? settings.weather
                 : Blend(weather_from_, settings.weather, SmoothStep(0.0f, 1.0f, blend_));

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

    if (settings.driveWeather && weather_ != wi::ecs::INVALID_ENTITY)
        ApplyWeather(scene);
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
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Day / Night & Weather", open)) {
        ImGui::End();
        return;
    }

    // The one line that is worth seeing whichever tab is open: what time it is, what the
    // weather is, and whether it is still on its way somewhere.
    ImGui::Checkbox("Enabled", &settings.enabled);
    ImGui::SameLine();
    ImGui::Text("%s  |  %s", ClockText().c_str(), ToString(preset_));
    if (WeatherBlend() > 0.0f) {
        ImGui::SameLine();
        ImGui::TextDisabled("changing %.0f%%", blend_ * 100.0f);
    }
    if (!settings.enabled)
        ImGui::TextDisabled("off leaves the light and the weather exactly as the scene left them");

    if (ImGui::BeginTabBar("daynight_tabs")) {
        if (ImGui::BeginTabItem("Time")) {
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
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Sun")) {
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
            ImGui::TextDisabled("The weather tab scales exposure and ambient on top of these.");
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Weather")) {
            ImGui::BeginDisabled(!settings.driveWeather);

            // Presets first, and as buttons rather than a combo: picking weather is the
            // common action, and a combo hides six of the seven choices behind a click.
            for (int i = 0; i < int(WeatherPreset::Count); ++i) {
                const WeatherPreset preset = WeatherPreset(i);
                if (i % 4 != 0) ImGui::SameLine();
                const bool active = (preset_ == preset);
                if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button(ToString(preset), ImVec2(84.0f, 0.0f)))
                    SetWeather(preset);
                if (active) ImGui::PopStyleColor();
            }
            ImGui::SliderFloat("Transition", &settings.transitionSeconds, 0.0f, 60.0f, "%.1f s");

            ImGui::SeparatorText("Automatic");
            ImGui::Checkbox("Change on its own", &settings.autoWeather);
            ImGui::BeginDisabled(!settings.autoWeather);
            ImGui::SliderFloat("Every", &settings.autoIntervalHours, 0.25f, 48.0f, "%.2f game hours");
            ImGui::TextDisabled("Next in %.2f h", std::max(0.0f, settings.autoIntervalHours - autoHours_));
            for (int i = 0; i < int(WeatherPreset::Count); ++i) {
                if (i % 4 != 0) ImGui::SameLine();
                bool allowed = (settings.autoPresets & (1u << i)) != 0u;
                if (ImGui::Checkbox(ToString(WeatherPreset(i)), &allowed)) {
                    if (allowed) settings.autoPresets |=  (1u << i);
                    else         settings.autoPresets &= ~(1u << i);
                }
            }
            int seed = int(settings.autoSeed);
            if (ImGui::InputInt("Seed", &seed)) {
                settings.autoSeed = uint32_t(std::max(1, seed));
                rng_ = 0u;   // reseed on the next pick, so a new seed takes effect at once
            }
            ImGui::EndDisabled();

            ImGui::SeparatorText("Cloud");
            ImGui::Checkbox("Volumetric clouds", &settings.clouds);
            ImGui::SameLine();
            ImGui::BeginDisabled(!settings.clouds);
            ImGui::Checkbox("Cast shadows", &settings.cloudShadows);
            ImGui::EndDisabled();
            ImGui::SliderFloat("Cloudiness", &settings.weather.cloudiness, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Cloud darkness", &settings.weather.cloudDarkness, 0.0f, 1.0f, "%.2f");

            ImGui::SeparatorText("Fog");
            ImGui::Checkbox("Height fog", &settings.heightFog);
            ImGui::SliderFloat("Density", &settings.weather.fogDensity, 0.0f, 0.5f, "%.3f");
            ImGui::SliderFloat("Starts at", &settings.weather.fogStart, 0.0f, 500.0f, "%.0f m");
            ImGui::BeginDisabled(!settings.heightFog);
            ImGui::SliderFloat("Band bottom", &settings.weather.fogHeightStart, -100.0f, 500.0f, "%.0f m");
            ImGui::SliderFloat("Band top",    &settings.weather.fogHeightEnd,   -100.0f, 500.0f, "%.0f m");
            ImGui::EndDisabled();

            ImGui::SeparatorText("Wind");
            ImGui::SliderFloat("Speed", &settings.weather.windSpeed, 0.0f, 40.0f, "%.1f m/s");
            ImGui::SliderFloat("Heading", &settings.weather.windDegrees, 0.0f, 360.0f, "%.0f deg");
            ImGui::SliderFloat("Turbulence", &settings.weather.windTurbulence, 0.0f, 20.0f, "%.1f");
            ImGui::SliderFloat("Wave size", &settings.weather.windWaveSize, 0.0f, 4.0f, "%.2f");

            ImGui::SeparatorText("Rain");
            ImGui::SliderFloat("Amount", &settings.weather.rain, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Fall speed", &settings.weather.rainSpeed, 0.1f, 4.0f, "%.2f");

            ImGui::SeparatorText("Light scaling");
            ImGui::SliderFloat("Exposure x", &settings.weather.exposureScale, 0.1f, 2.0f, "%.2f");
            ImGui::SliderFloat("Ambient x",  &settings.weather.ambientScale,  0.1f, 2.0f, "%.2f");
            ImGui::TextDisabled("Sliders apply at once. Presets ease in over the transition time.");

            ImGui::EndDisabled();
            if (!settings.driveWeather)
                ImGui::TextDisabled("\"Drive the weather\" is off on the Sun tab, so none of this is written.");
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Target")) {
            if (HasSun()) {
                ImGui::Text("Sun entity     %llu%s", (unsigned long long)sun_,
                            owned_ ? "  (created here)" : "  (adopted)");
                ImGui::Text("Weather entity %llu%s", (unsigned long long)weather_,
                            weather_ == wi::ecs::INVALID_ENTITY ? "  (none - weather is not driven)" : "");
                ImGui::Text("Direction      %.3f %.3f %.3f",
                            sun_position_.direction.x, sun_position_.direction.y, sun_position_.direction.z);
                ImGui::Text("Azimuth        %.1f deg from north", Degrees(sun_position_.azimuth));
                ImGui::Text("Wind           %.1f m/s towards %.0f deg",
                            weather_now_.windSpeed, weather_now_.windDegrees);
                ImGui::Text("Rain           %.0f%%   cloud %.0f%%   fog %.3f",
                            weather_now_.rain * 100.0f, weather_now_.cloudiness * 100.0f,
                            weather_now_.fogDensity);
                if (ImGui::Button("Detach")) Detach();
            } else {
                ImGui::TextDisabled("Nothing attached. AppConfig::dayNight decides whether a scene\n"
                                    "load binds one; Off means the game does it itself.");
                if (ImGui::Button("Adopt the scene's sun")) Adopt(wi::scene::GetScene());
                ImGui::SameLine();
                if (ImGui::Button("Create one")) Create(wi::scene::GetScene());
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
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

    out.putFloat("transitionSeconds", s.transitionSeconds);
    out.putBool ("clouds", s.clouds);
    out.putBool ("cloudShadows", s.cloudShadows);
    out.putBool ("heightFog", s.heightFog);
    out.putBool ("autoWeather", s.autoWeather);
    out.putFloat("autoIntervalHours", s.autoIntervalHours);
    out.putInt  ("autoPresets", int(s.autoPresets));
    out.putInt  ("autoSeed", int(s.autoSeed));
    // The preset is stored as well as the state it expands to: a save that names "Storm"
    // still says Storm after the preset's numbers are retuned, where a bag of floats
    // would silently become a custom sky that no longer matches the tuned one.
    out.putInt  ("weatherPreset", int(preset_));
    out.putFloat("wCloudiness", s.weather.cloudiness);
    out.putFloat("wCloudDarkness", s.weather.cloudDarkness);
    out.putFloat("wExposureScale", s.weather.exposureScale);
    out.putFloat("wAmbientScale", s.weather.ambientScale);
    out.putFloat("wFogDensity", s.weather.fogDensity);
    out.putFloat("wFogStart", s.weather.fogStart);
    out.putFloat("wFogHeightStart", s.weather.fogHeightStart);
    out.putFloat("wFogHeightEnd", s.weather.fogHeightEnd);
    out.putFloat("wWindSpeed", s.weather.windSpeed);
    out.putFloat("wWindDegrees", s.weather.windDegrees);
    out.putFloat("wWindTurbulence", s.weather.windTurbulence);
    out.putFloat("wWindWaveSize", s.weather.windWaveSize);
    out.putFloat("wRain", s.weather.rain);
    out.putFloat("wRainSpeed", s.weather.rainSpeed);
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

    s.transitionSeconds   = in.getFloat("transitionSeconds", s.transitionSeconds);
    s.clouds              = in.getBool ("clouds", s.clouds);
    s.cloudShadows        = in.getBool ("cloudShadows", s.cloudShadows);
    s.heightFog           = in.getBool ("heightFog", s.heightFog);
    s.autoWeather         = in.getBool ("autoWeather", s.autoWeather);
    s.autoIntervalHours   = in.getFloat("autoIntervalHours", s.autoIntervalHours);
    s.autoPresets         = uint32_t(in.getInt("autoPresets", int(s.autoPresets)));
    s.autoSeed            = uint32_t(in.getInt("autoSeed", int(s.autoSeed)));
    s.weather.cloudiness     = in.getFloat("wCloudiness", s.weather.cloudiness);
    s.weather.cloudDarkness  = in.getFloat("wCloudDarkness", s.weather.cloudDarkness);
    s.weather.exposureScale  = in.getFloat("wExposureScale", s.weather.exposureScale);
    s.weather.ambientScale   = in.getFloat("wAmbientScale", s.weather.ambientScale);
    s.weather.fogDensity     = in.getFloat("wFogDensity", s.weather.fogDensity);
    s.weather.fogStart       = in.getFloat("wFogStart", s.weather.fogStart);
    s.weather.fogHeightStart = in.getFloat("wFogHeightStart", s.weather.fogHeightStart);
    s.weather.fogHeightEnd   = in.getFloat("wFogHeightEnd", s.weather.fogHeightEnd);
    s.weather.windSpeed      = in.getFloat("wWindSpeed", s.weather.windSpeed);
    s.weather.windDegrees    = in.getFloat("wWindDegrees", s.weather.windDegrees);
    s.weather.windTurbulence = in.getFloat("wWindTurbulence", s.weather.windTurbulence);
    s.weather.windWaveSize   = in.getFloat("wWindWaveSize", s.weather.windWaveSize);
    s.weather.rain           = in.getFloat("wRain", s.weather.rain);
    s.weather.rainSpeed      = in.getFloat("wRainSpeed", s.weather.rainSpeed);
    settings = s;

    // A named preset that was saved is re-applied by NAME, so it picks up the current
    // tuning; anything else keeps the numbers that were stored. Either way it is on
    // screen immediately - a settings load is not a weather change to watch happen.
    const int preset = in.getInt("weatherPreset", int(WeatherPreset::Count));
    if (preset >= 0 && preset < int(WeatherPreset::Count)) SetWeather(WeatherPreset(preset), 0.0f);
    else                                                   SetWeather(settings.weather, 0.0f);
}

} // namespace st
