#pragma once
// EosPlatform - Epic Online Services for a Simtary game (Simtary/ONLINE.md).
//
// Owns the SDK's one platform: EOS_Initialize + EOS_Platform_Create at start, EOS_Platform_Tick
// once a frame on the main thread (every EOS callback fires inside that tick, so callbacks run
// on the main thread too), release + EOS_Shutdown at exit. The SDK can only be initialized once
// per process.
//
// The project supplies the ids (AppConfig::eos, EosConfig.h). Command line / environment
// overrides, mainly for a dedicated server whose credentials must never ship in a client:
//
//   --eos-client-id=<id>       or EOS_CLIENT_ID
//   --eos-client-secret=<s>    or EOS_CLIENT_SECRET
//   --eos-sandbox=<id>         --eos-deployment=<id>
//   --no-eos                   do not start EOS at all
//
// A headless run (--server) creates the platform as a SERVER (bIsServer) and logs nobody in;
// every other run logs in with the Device ID: create one for this machine on first use, log in,
// and create the EOS user on the first login - the result is a ProductUserId.
//
// Built only with the SDK in deps/eos-sdk (SIMTARY_HAS_EOS); otherwise everything here is a
// no-op and Status().compiled is false.

#include "online/EosConfig.h"

#include <string>

namespace st::online {

enum class EosState {
    Off,            // not compiled, not configured, or --no-eos
    Failed,         // initialize / platform create / login failed - see Status().error
    Ready,          // platform up, nobody logged in (a server, or autoLogin off)
    LoggingIn,
    LoggedIn,
};

struct EosStatus {
    bool compiled = false;
    bool server = false;
    EosState state = EosState::Off;
    std::string productUserId;   // after login
    std::string error;           // last failure, EOS_EResult name + where
};

class EosPlatform {
public:
    static EosPlatform& Get ();

    // productName / productVersion are what the SDK reports to Epic.
    void Initialize (const EosConfig& config, const std::string& productName, const std::string& productVersion);
    void Update ();
    void Shutdown ();

    // Device ID login (client only). Done automatically when EosConfig::autoLogin is on.
    void LoginDevice ();

    const EosStatus& Status () const { return status_; }
    bool IsLoggedIn () const { return status_.state == EosState::LoggedIn; }

    // EOS_HPlatform, as void* so this header does not pull in the SDK. Null when off.
    void* Handle () const { return platform_; }

    static const char* StateName (EosState state);

private:
    friend struct EosLoginFlow;   // the SDK's Connect callbacks (EosPlatform.cpp)
    void RegisterBridge ();

    EosConfig config_;
    EosStatus status_;
    void* platform_ = nullptr;
    bool sdkInitialized_ = false;
};

} // namespace st::online
