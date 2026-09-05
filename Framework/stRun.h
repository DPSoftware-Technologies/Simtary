#pragma once
// st::Run - the Simtary main loop.
//
// The project owns main(); this owns everything after it. A project's
// src/main.cpp fills an AppConfig with its properties, constructs its App
// subclass and hands both over:
//
//     int main (int argc, char* argv[]) {
//         st::AppConfig config;
//         config.name         = "MyGame";
//         config.startupScene = "Scene1";
//         MyGame app;
//         return st::Run(argc, argv, config, app);
//     }
//
// Run() installs the config (App::Config(), the user-data folder, the crash
// reporter), opens the window, shows the splash, drives Initialize/Run/Exit and
// pumps SDL events until the window closes.
#include "stApp.h"

namespace st {

// `config` must outlive the call: the framework keeps a pointer to it.
// Returns the process exit code.
int Run(int argc, char* argv[], AppConfig& config, App& app);

} // namespace st
