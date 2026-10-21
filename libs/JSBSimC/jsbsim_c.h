/* jsbsim_c - a plain C interface to JSBSim, built as its own shared library (SimtaryJSBSim).
 *
 * Why a DLL with a C surface:
 *   - JSBSim is LGPL 2.1. Shipping it as a separate, replaceable shared library is the plain way
 *     to meet that licence; this shim is part of that library and shares its licence.
 *   - JSBSim throws (a malformed aircraft file is a JSBSim::BaseException) and uses RTTI. The
 *     engine is built without either, so nothing C++ from JSBSim may cross into it. Every call
 *     here catches everything and reports through stj_last_error().
 *
 * Model files: JSBSim reads an aircraft from <root>/aircraft/<model>/<model>.xml, and engines
 * and rotors from <root>/engine/. Simtary generates these (Framework/flight) - nobody writes
 * them by hand.
 *
 * Everything else goes through JSBSim's property tree by name: initial conditions
 * ("ic/h-sl-ft"), controls ("propulsion/engine[0]/collective-ctrl-rad"), state
 * ("position/lat-geod-deg", "attitude/psi-rad", "propulsion/engine[0]/rotor-rpm").
 *
 * Ground: JSBSim's own is a smooth ellipsoid at sea level. stj_set_ground hands it a function
 * that returns the terrain height under a latitude / longitude, so contacts land on the game's
 * terrain.
 *
 * One thread per instance. Plain C, no exceptions cross it.
 */
#ifndef JSBSIM_C_H
#define JSBSIM_C_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  ifdef STJ_BUILDING
#    define STJ_API __declspec(dllexport)
#  else
#    define STJ_API __declspec(dllimport)
#  endif
#else
#  define STJ_API __attribute__((visibility("default")))
#endif

typedef struct stj_fdm stj_fdm;

/* Terrain height above sea level in METRES at a geodetic latitude / longitude (degrees). */
typedef double (*stj_ground_fn)(void* user, double lat_deg, double lon_deg);

/* Loads <root>/aircraft/<model>/<model>.xml. `dt` is the fixed step in seconds (JSBSim likes
 * 1/120 or smaller). NULL with `error` filled on failure. */
STJ_API stj_fdm* stj_create (const char* root_dir, const char* model, double dt, char* error, size_t error_size);
STJ_API void stj_destroy (stj_fdm* fdm);

STJ_API void stj_set_ground (stj_fdm* fdm, stj_ground_fn fn, void* user);

/* Property access. stj_set returns 0 when the property does not exist; stj_get sets *found
 * (when not NULL) to 0 and returns 0.0 for a missing property. */
STJ_API int stj_set (stj_fdm* fdm, const char* property, double value);
STJ_API double stj_get (stj_fdm* fdm, const char* property, int* found);

/* Applies the ic/ properties set so far and starts the model there (RunIC). 0 on failure. */
STJ_API int stj_init (stj_fdm* fdm);
/* One step of `dt`. 0 when JSBSim failed or stopped (see stj_last_error). */
STJ_API int stj_run (stj_fdm* fdm);
/* Several steps; returns how many ran. */
STJ_API int stj_run_steps (stj_fdm* fdm, int steps);

STJ_API const char* stj_last_error (const stj_fdm* fdm);
STJ_API const char* stj_version (void);

#ifdef __cplusplus
}
#endif

#endif /* JSBSIM_C_H */
