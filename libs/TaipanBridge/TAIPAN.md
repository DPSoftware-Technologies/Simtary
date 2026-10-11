# Taipan (git submodule)

`libs/Taipan` is a git submodule of
https://github.com/DPSoftware-Technologies/TaipanLang, pinned like the other submodules
(ImGui, SDL2, ...) by the commit Simtary records. Simtary carries **no local patches**:
what it needed went upstream.

| | |
|---|---|
| Pinned | `dccd3e9` - 0.2.0 + `taipan_set_file_reader` (branch `host-file-reader`) |
| Needed from upstream | `taipan_set_file_reader`, value types / operators / statics (0.2.0) |

`taipan_set_file_reader` is what lets scripts - and their dotted imports - load out of
the asset package (`Framework/script/ScriptSystem.cpp` points it at
`wi::helper::FileRead`).

Only the sources are built: `libs/TaipanBridge/CMakeLists.txt` lists
`libs/Taipan/src/*.cpp` and never `add_subdirectory()`s the upstream project, so its
runner, language server, VS Code extension and tests stay out of the game build.

## Why it is not reached directly

Taipan reports every script error with a C++ exception and passes `std::string` /
`std::function` through its public header. This workspace builds games with
`/EHsc- /GR- _HAS_EXCEPTIONS=0`, so - exactly like JSBSim - `taipan.h` must never be
included from engine or framework code. The bridge (`StTaipan.cpp`, `StTaipanMath.cpp`)
is the only code that does, compiled with Taipan's own settings, and it exposes a POD,
non-throwing surface (`StTaipan.h`) to the rest of the framework. The CMake target
`SimtaryScript` builds both and publishes only this directory's include path.

The bridge also reads two internal tables (`internal.h`, `loader.h`): the VM's native
signatures for editor completion (`ListSymbols`) and a script's declared globals for
inspector fields (`ListFields`). Re-check those two after a bump.

## Updating

```
git -C libs/Taipan fetch
git -C libs/Taipan checkout <tag or commit>
git add libs/Taipan
```

Then build `script_bridge_test` and run it (exceptions-off host against the bridge), and
build Milistry. If upstream changed `ModuleSymbols` / `NativeClass` / the loader entry
table, `StTaipan.cpp` is where it shows.

## Upstream behaviour the bridge works around

- `taipan_load_string` with a chunk name that was used before re-runs the FIRST
  chunk's code (modules are cached by key). `st::taipan::RunString` numbers every chunk
  (`console#1`, `#2`, ...).
- `start` is a keyword (it launches a coroutine), so a script cannot define
  `func start()`. stScript's callbacks are Unity's `Start` / `Update` / ... (and the 0.1
  `onStart` / `onUpdate` / ... names).
