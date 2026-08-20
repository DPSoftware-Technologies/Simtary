# Faust DSP (AOT) integration

Ahead-of-time [Faust](https://faust.grame.fr/) DSP: the `faust` compiler turns a
`.dsp` program into a plain C++ `dsp` subclass, which the app plays through the
engine's OpenAL output stream. No LLVM and no `libfaust` at runtime.

This folder (`assets/signal_descriptors/`) holds the Faust signal descriptors
(`.dsp`) and the codegen template. The generated C++ lives in the source tree
under `src/audio/faust/processor/`.

## Files

| Path | Role |
|------|------|
| `organ.dsp` | Faust source for the demo instrument (additive organ tone). |
| `faust_arch.h` | Faust architecture template used during regeneration. |
| `../../src/audio/faust/processor/organ.gen.h` | Generated C++ (`class OrganDSP : public dsp`). **Checked in** so the build works without the compiler. |
| `../../include/faust/**` | Vendored Faust base ABI headers (`dsp.h`/`UI.h`/`meta.h`/`export.h`). |
| `../../src/audio/faust/FaustProcessor.h` | Generic adapter: `FaustProcessor<DSP>` wraps a Faust `dsp` as a `wi::audio::DSPSource`. |
| `../../src/audio/faust/FaustManager.*` | Load/unload registry, OpenAL playback, and the ImGui panel. |
| `../../Simtary/stAudio.*` | Engine OpenAL audio subsystem: `wi::audio::DSPStream` streaming output. |

## Runtime

`Milistry` menu → **HMMWV4 → Faust DSP** opens the manager panel: a processor
picker (Load/Unload), master gain, and a slider per Faust control (`freq`,
`gain`). Audio is rendered on a worker thread inside `wi::audio::DSPStream`
(OpenAL Soft, shipped as `OpenAL32.dll` next to the executable) — independent of
the legacy FAudio sound-effect path in `wiAudio.cpp`, which `stAudio` is intended
to eventually replace.

## Adding another processor

1. Write `mysynth.dsp` here and regenerate with a **unique** class name:
   ```
   faust -lang cpp -cn MySynthDSP -a assets/signal_descriptors/faust_arch.h \
         -I libs/faust/libraries \
         -o src/audio/faust/processor/mysynth.gen.h \
         assets/signal_descriptors/mysynth.dsp
   ```
2. In `src/audio/faust/FaustManager.cpp`, `#include` the header and register it:
   ```cpp
   Register("mysynth", []{ return std::make_unique<FaustProcessor<MySynthDSP>>("mysynth"); });
   ```
   It then appears in the panel's processor picker.

## Regenerating `src/audio/faust/processor/organ.gen.h`

The checked-in header is a hand-written stand-in with the exact same public `dsp`
ABI as real Faust output, so regenerated code drops in with no other changes.

1. Install a **prebuilt** `faust` (building it from `libs/faust` needs LLVM):
   - Windows: the installer from <https://github.com/grame-cncm/faust/releases>
   - or a package manager: `winget install`, MSYS2 `pacman -S mingw-w64-ucrt-x86_64-faust`, `brew install faust`, `apt install faust`
2. Re-run CMake (`cmake -B build ...`). It detects `faust` and defines the target.
3. Regenerate:
   ```
   cmake --build build --target faust_regen
   ```
   or directly (from the repo root):
   ```
   faust -lang cpp -cn OrganDSP -a assets/signal_descriptors/faust_arch.h \
         -I libs/faust/libraries \
         -o src/audio/faust/processor/organ.gen.h \
         assets/signal_descriptors/organ.dsp
   ```

Editing `organ.dsp` (e.g. a different synth) then running `faust_regen` and
rebuilding is the whole authoring loop.
