// jsbsim_c - see jsbsim_c.h. Built WITH exceptions and RTTI, like JSBSim itself; nothing of
// either leaves this file.
#define STJ_BUILDING
#include "jsbsim_c.h"

#include "FGFDMExec.h"
#include "input_output/FGGroundCallback.h"
#include "input_output/FGPropertyManager.h"
#include "math/FGLocation.h"
#include "math/FGColumnVector3.h"
#include "models/FGInertial.h"
#include "simgear/misc/sg_path.hxx"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

using namespace JSBSim;

struct stj_fdm {
    FGFDMExec* exec = nullptr;
    std::string error;
    stj_ground_fn ground = nullptr;
    void* groundUser = nullptr;
};

namespace {

constexpr double kFeetPerMetre = 3.280839895;

void Copy (char* dst, size_t n, const std::string& s) {
    if (dst == nullptr || n == 0) return;
    std::snprintf(dst, n, "%s", s.c_str());
}

// JSBSim's sea-level ellipsoid, lifted to the game's terrain under the aircraft. The normal is
//	the ellipsoid's (local up): fine for skids on gentle ground.
class TerrainGround final : public FGGroundCallback {
public:
    TerrainGround (const stj_fdm* fdm, double a, double b) : fdm_(fdm), a_(a), b_(b) {}

    double GetAGLevel (double, const FGLocation& loc, FGLocation& contact, FGColumnVector3& normal,
                       FGColumnVector3& vel, FGColumnVector3& angularVel) const override {
        vel.InitMatrix();
        angularVel.InitMatrix();
        FGLocation l = loc;
        l.SetEllipse(a_, b_);
        const double latitude = l.GetGeodLatitudeRad();
        const double longitude = l.GetLongitude();
        const double cosLat = std::cos(latitude);
        normal = FGColumnVector3(cosLat * std::cos(longitude), cosLat * std::sin(longitude), std::sin(latitude));
        double elevationFt = 0.0;
        if (fdm_->ground) {
            const double metres = fdm_->ground(fdm_->groundUser, latitude * 180.0 / M_PI, longitude * 180.0 / M_PI);
            if (std::isfinite(metres)) elevationFt = metres * kFeetPerMetre;
        }
        contact.SetEllipse(a_, b_);
        contact.SetPositionGeodetic(longitude, latitude, elevationFt);
        return l.GetGeodAltitude() - elevationFt;
    }

    void SetEllipse (double semimajor, double semiminor) override { a_ = semimajor; b_ = semiminor; }

private:
    const stj_fdm* fdm_;
    double a_, b_;
};

template <typename Fn>
bool Guard (stj_fdm* fdm, Fn&& fn) {
    try {
        fn();
        return true;
    } catch (const std::exception& e) {
        if (fdm) fdm->error = e.what();
    } catch (const std::string& s) {
        if (fdm) fdm->error = s;
    } catch (const char* s) {
        if (fdm) fdm->error = s ? s : "unknown JSBSim error";
    } catch (...) {
        if (fdm) fdm->error = "unknown JSBSim error";
    }
    return false;
}

} // namespace

extern "C" {

stj_fdm* stj_create (const char* root_dir, const char* model, double dt, char* error, size_t error_size) {
    stj_fdm* fdm = new stj_fdm();
    const bool ok = Guard(fdm, [&] {
        // Quiet unless asked: JSBSim prints a banner and the whole model at its default level
        //	(read from JSBSIM_DEBUG by the FGFDMExec constructor). JSBSIM_DEBUG=1 restores it.
        const bool quiet = std::getenv("JSBSIM_DEBUG") == nullptr;
#ifdef _WIN32
        if (quiet) _putenv_s("JSBSIM_DEBUG", "0");
#else
        if (quiet) setenv("JSBSIM_DEBUG", "0", 0);
#endif
        fdm->exec = new FGFDMExec();
        fdm->exec->SetRootDir(SGPath(root_dir ? root_dir : "."));
        fdm->exec->SetAircraftPath(SGPath("aircraft"));
        fdm->exec->SetEnginePath(SGPath("engine"));
        fdm->exec->SetSystemsPath(SGPath("systems"));
        fdm->exec->Setdt(dt > 0 ? dt : 1.0 / 120.0);
        if (!fdm->exec->LoadModel(model ? model : "")) throw std::string("JSBSim could not load model '") + (model ? model : "") + "'";
    });
    if (!ok) {
        Copy(error, error_size, fdm->error);
        delete fdm->exec;
        delete fdm;
        return nullptr;
    }
    return fdm;
}

void stj_destroy (stj_fdm* fdm) {
    if (fdm == nullptr) return;
    Guard(fdm, [&] { delete fdm->exec; });
    delete fdm;
}

void stj_set_ground (stj_fdm* fdm, stj_ground_fn fn, void* user) {
    if (fdm == nullptr) return;
    fdm->ground = fn;
    fdm->groundUser = user;
    Guard(fdm, [&] {
        auto inertial = fdm->exec->GetInertial();
        inertial->SetGroundCallback(new TerrainGround(fdm, inertial->GetSemimajor(), inertial->GetSemiminor()));
    });
}

int stj_set (stj_fdm* fdm, const char* property, double value) {
    if (fdm == nullptr || property == nullptr) return 0;
    int found = 0;
    Guard(fdm, [&] {
        auto node = fdm->exec->GetPropertyManager()->GetNode(property);
        if (!node) return;
        node->setDoubleValue(value);
        found = 1;
    });
    return found;
}

double stj_get (stj_fdm* fdm, const char* property, int* found) {
    if (found) *found = 0;
    if (fdm == nullptr || property == nullptr) return 0.0;
    double v = 0.0;
    Guard(fdm, [&] {
        auto node = fdm->exec->GetPropertyManager()->GetNode(property);
        if (!node) return;
        v = node->getDoubleValue();
        if (found) *found = 1;
    });
    return v;
}

int stj_init (stj_fdm* fdm) {
    if (fdm == nullptr) return 0;
    bool ran = false;
    const bool ok = Guard(fdm, [&] { ran = fdm->exec->RunIC(); });
    if (ok && !ran) fdm->error = "RunIC failed";
    return ok && ran ? 1 : 0;
}

int stj_run (stj_fdm* fdm) {
    if (fdm == nullptr) return 0;
    bool ran = false;
    const bool ok = Guard(fdm, [&] { ran = fdm->exec->Run(); });
    if (ok && !ran && fdm->error.empty()) fdm->error = "JSBSim stopped";
    return ok && ran ? 1 : 0;
}

int stj_run_steps (stj_fdm* fdm, int steps) {
    int n = 0;
    while (n < steps && stj_run(fdm)) ++n;
    return n;
}

const char* stj_last_error (const stj_fdm* fdm) { return fdm ? fdm->error.c_str() : "no fdm"; }

const char* stj_version (void) { return FGJSBBase::GetVersion().c_str(); }

} // extern "C"
