#pragma once
// Script Editor - edit, check and save Taipan scripts inside the running game.
//
// Opened from Simtary > Script Editor, from "Edit" on an stScript in the inspector, or
// by double-clicking a .tp in the Resource Explorer. One tab per script.
//
//	type       the script is re-checked (parse + type check, nothing runs) shortly after
//	           you stop typing; errors and warnings are listed under the text, click one
//	           to jump to it
//	Ctrl+S     save: writes the AUTHORING copy (assets/contents/... in the project's
//	           source tree, so the next build packs it), stands the text in for the
//	           packaged copy for the rest of the session, and hot-reloads every entity
//	           running the file - globals keep their values, functions are replaced
//	New        a script from a template with all four stScript callbacks
//
// Saving is safe to do while the scene plays: a script that does not compile is still
// written (it is your file), but the running instances keep the last good code until
// it does - the save just reports the errors.

#include <string>

namespace st::devui {

// Open `logicalPath` ("assets/scripts/ai.tp") in a tab - or focus it if it is already
// open - and show the window.
void OpenScriptEditor (const std::string& logicalPath);

// The DevUI menu's toggle for the window.
bool& ScriptEditorVisible ();

// Draws the window when it is visible.
void ScriptEditorWindow ();

} // namespace st::devui
