#pragma once

// Offline crash reporting for a Simtary app, backed by the sentry-native SDK
// (Crashpad backend). Minidumps are written to a local "crashreports" folder
// next to the executable; nothing is uploaded to any server. On a crash a small
// cross-platform reporter GUI (SimtaryCrashReporter) is launched to tell the
// user where the crash happened and where the dump was saved.
namespace st {
namespace crash {

// Initialize the crash handler. Call once, as the very first thing in main(),
// before any other subsystem so crashes during startup are captured too.
// `appName` is shown in the crash summary and the reporter GUI.
void Init(const char* exePath, const char* appName = "Simtary");

// If the previous run crashed but the reporter was not shown (e.g. on_crash
// could not launch it), show it now. Call once right after Init().
void CheckPreviousCrash();

// Flush and shut the SDK down cleanly. Call once before the process exits.
void Shutdown();

} // namespace crash
} // namespace st
