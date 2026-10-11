#pragma once
// Taipan Console: the DevUI window for the framework's script VM (st::ScriptSystem).
//
// Runs a chunk of Taipan in the VM's main environment - the same engine API every
// stScript sees (scene.find, log.*, Entity) - so a binding can be poked at without
// writing a file. Output and diagnostics go to the backlog, as everything else does;
// the window repeats only the last error so a typo is visible without switching panels.
//
// "Write bindings stub" saves the registered API as taipan_bindings.tpi in the working
// directory. Copy it next to the scripts and the Taipan language server type-checks
// them against the real engine API.

namespace st::devui {

// `show` is the DevUI menu's toggle.
void ScriptConsoleWindow (bool* show);

} // namespace st::devui
