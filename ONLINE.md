# Online services (Epic Online Services)

`Framework/online/` - Epic Online Services for any Simtary game. The framework holds no ids;
each project brings its own product from the Epic Developer Portal.

## Setup

1. Unzip the EOS C SDK's `SDK/` folder to `Simtary/deps/eos-sdk/` (`Include/`, `Lib/`, `Bin/`).
   CMake finds it (`SimtaryEOS`, `SIMTARY_HAS_EOS`) and copies `EOSSDK-Win64-Shipping.dll`
   (Linux: `libEOSSDK-Linux-Shipping.so`) next to the exe. Without it everything is a stub.
2. Project `main.cpp` fills `config.eos` (`EosConfig.h`): productId, sandboxId, deploymentId,
   clientId. Empty productId = EOS off.
3. The client secret is NOT committed: Milistry reads it from `src/eos_secret.h` (gitignored):
   ```cpp
   #define MILISTRY_EOS_CLIENT_SECRET "..."
   ```
   Creating the file does NOT rebuild `main.cpp` (MSBuild never saw it as a dependency while it
   was missing): touch `main.cpp` once, or `eos.status` keeps saying "incomplete configuration".
4. A dedicated server uses the SERVER client (GameServer policy), never the game client's:
   `--eos-client-id=` / `--eos-client-secret=` or `EOS_CLIENT_ID` / `EOS_CLIENT_SECRET`.
   `--eos-sandbox=` / `--eos-deployment=` switch Dev / Stage / Live. `--no-eos` skips it.

## What runs

- `EosPlatform::Initialize` (from `st::App::Initialize`, after NetSession): `EOS_Initialize`,
  `EOS_Platform_Create` (cache in `<userdata>/eos/`, overlay disabled, `bIsServer` on a headless
  run), SDK log -> backlog as `EOS: [category] ...` (warnings and up).
- `Update` = `EOS_Platform_Tick`, once a frame on the main thread; every callback fires there.
- Client, `autoLogin`: Connect login with the Device ID. `NotFound` -> `CreateDeviceId` -> login
  again; `InvalidUser` -> `CreateUser` with the continuance token. Result: a ProductUserId.
- `Shutdown`: release + `EOS_Shutdown` (final - the SDK cannot be initialized twice per process).
- Bridge (dev build): `eos.status`, `eos.login`.

## Next

Server registers a Session; client server browser; sanctions check at join (server); own
OpenID provider for accounts; Easy Anti-Cheat; overlay (needs swapchain hook).
