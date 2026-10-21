#pragma once
// What a PROJECT tells the framework about its Epic Online Services product. The framework
// holds no ids of its own: a game fills AppConfig::eos in its main.cpp, and EOS stays off
// while productId is empty. Ids come from the Epic Developer Portal (Product Settings ->
// SDK Download & Credentials, and the Clients tab).
//
// Secrets do not belong in source control. The game client's secret ships inside the game
// anyway (that is why its client policy is the limited GameClient one), so a project bakes
// it from a gitignored header; a dedicated server reads its own client id + secret from the
// command line or the environment instead, see EosPlatform.h.

#include <string>

namespace st::online {

struct EosConfig {
    std::string productId;
    std::string sandboxId;      // Dev / Stage / Live - one per sandbox
    std::string deploymentId;   // the deployment that belongs to sandboxId
    std::string clientId;       // the GameClient client
    std::string clientSecret;

    // Client builds log in with the Device ID (no account, no UI) as soon as the platform
    // is up. Off = the game calls EosPlatform::LoginDevice() itself.
    bool autoLogin = true;

    // Shown on Epic's side next to the user (Connect UserLoginInfo, max 32 characters).
    // Empty = the AppConfig name.
    std::string displayName;
};

} // namespace st::online
