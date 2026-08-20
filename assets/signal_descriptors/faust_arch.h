/* faust_arch.h — Faust architecture template for AOT code generation.
 *
 * `faust -a assets/signal_descriptors/faust_arch.h` substitutes the generated DSP
 * into the <<includeclass>> placeholder below, emitting a self-contained header
 * that pulls in the Faust base ABI (dsp/UI/Meta) and defines a Faust dsp subclass
 * named by faust's -cn flag (e.g. `class OrganDSP : public dsp`). The manager
 * (src/audio/faust/) wraps it via FaustProcessor<OrganDSP> and drives it as a
 * wi::audio::DSPSource.
 *
 * The checked-in src/audio/faust/processor/organ.gen.h mirrors this layout so the
 * build works before `faust` is installed; regeneration overwrites it in place.
 */
#include <faust/dsp/dsp.h>
#include <faust/gui/UI.h>
#include <faust/gui/meta.h>

#include <cmath>

<<includeIntrinsic>>

<<includeclass>>
