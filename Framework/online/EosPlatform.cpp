#include "online/EosPlatform.h"

#include "dev/DevBuild.h"
#include "dev/EditorBridge.h"
#include "io/UserData.h"
#include "stRun.h"

#include "wiArguments.h"
#include "wiBacklog.h"

#include <cstdlib>
#include <filesystem>

#if defined(SIMTARY_HAS_EOS)
#include <eos_sdk.h>
#include <eos_init.h>
#include <eos_logging.h>
#include <eos_connect.h>
#endif

namespace st::online {

EosPlatform& EosPlatform::Get () {
    static EosPlatform instance;
    return instance;
}

const char* EosPlatform::StateName (EosState state) {
    switch (state) {
    case EosState::Off:       return "off";
    case EosState::Failed:    return "failed";
    case EosState::Ready:     return "ready";
    case EosState::LoggingIn: return "logging in";
    case EosState::LoggedIn:  return "logged in";
    }
    return "?";
}

namespace {

void Log (const std::string& msg, wi::backlog::LogLevel level = wi::backlog::LogLevel::Default) {
    wi::backlog::post("EOS: " + msg, level);
}

// --name=value on the command line, else the environment variable, else what the project set.
std::string Override (const char* arg, const char* env, const std::string& fallback) {
    std::string value;
    if (wi::arguments::GetArgumentValue(arg, value) && !value.empty()) return value;
    if (env != nullptr) {
        if (const char* v = std::getenv(env); v != nullptr && v[0] != '\0') return v;
    }
    return fallback;
}

#if defined(SIMTARY_HAS_EOS)

std::string ResultName (EOS_EResult result) {
    const char* name = EOS_EResult_ToString(result);
    return name != nullptr ? name : std::to_string(int(result));
}

void EOS_CALL OnLogMessage (const EOS_LogMessage* message) {
    if (message == nullptr || message->Message == nullptr) return;
    wi::backlog::LogLevel level = wi::backlog::LogLevel::Default;
    if (message->Level <= EOS_ELogLevel::EOS_LOG_Error) level = wi::backlog::LogLevel::Error;
    else if (message->Level <= EOS_ELogLevel::EOS_LOG_Warning) level = wi::backlog::LogLevel::Warning;
    Log(std::string("[") + (message->Category ? message->Category : "") + "] " + message->Message, level);
}

#endif

} // namespace

#if defined(SIMTARY_HAS_EOS)

// The Connect flow, as one small state machine driven by the SDK's callbacks:
//   Login(device id) -> NotFound      -> CreateDeviceId -> Login again
//                    -> InvalidUser   -> CreateUser(continuance token)
//                    -> Success       -> logged in
struct EosLoginFlow {
    static void Login (EosPlatform& eos);
    static void EOS_CALL OnLogin (const EOS_Connect_LoginCallbackInfo* data);
    static void EOS_CALL OnCreateDeviceId (const EOS_Connect_CreateDeviceIdCallbackInfo* data);
    static void EOS_CALL OnCreateUser (const EOS_Connect_CreateUserCallbackInfo* data);
    static void SetLoggedIn (EOS_ProductUserId user);
    static void SetFailed (const std::string& where, EOS_EResult result);
    static EOS_HConnect Connect () { return EOS_Platform_GetConnectInterface(EOS_HPlatform(EosPlatform::Get().platform_)); }

    static bool createdDevice;
    static std::string displayName;
};
bool EosLoginFlow::createdDevice = false;
std::string EosLoginFlow::displayName;

void EosLoginFlow::SetLoggedIn (EOS_ProductUserId user) {
    EosStatus& status = EosPlatform::Get().status_;
    char buffer[EOS_PRODUCTUSERID_MAX_LENGTH + 1] = {};
    int32_t length = int32_t(sizeof(buffer));
    status.productUserId = EOS_ProductUserId_ToString(user, buffer, &length) == EOS_EResult::EOS_Success ? buffer : "";
    status.state = EosState::LoggedIn;
    status.error.clear();
    Log("logged in, ProductUserId " + status.productUserId);
}

void EosLoginFlow::SetFailed (const std::string& where, EOS_EResult result) {
    EosStatus& status = EosPlatform::Get().status_;
    status.state = EosState::Failed;
    status.error = where + ": " + ResultName(result);
    Log(status.error, wi::backlog::LogLevel::Error);
}

void EosLoginFlow::Login (EosPlatform&) {
    EOS_Connect_Credentials credentials = {};
    credentials.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST;
    credentials.Type = EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN;
    credentials.Token = nullptr;   // the device id needs no token

    EOS_Connect_UserLoginInfo info = {};
    info.ApiVersion = EOS_CONNECT_USERLOGININFO_API_LATEST;
    info.DisplayName = displayName.c_str();

    EOS_Connect_LoginOptions options = {};
    options.ApiVersion = EOS_CONNECT_LOGIN_API_LATEST;
    options.Credentials = &credentials;
    options.UserLoginInfo = &info;
    EOS_Connect_Login(Connect(), &options, nullptr, &EosLoginFlow::OnLogin);
}

void EosLoginFlow::OnLogin (const EOS_Connect_LoginCallbackInfo* data) {
    if (data->ResultCode == EOS_EResult::EOS_Success) {
        SetLoggedIn(data->LocalUserId);
    } else if (data->ResultCode == EOS_EResult::EOS_NotFound && !createdDevice) {
        // No device id on this machine yet (it lives in the user's local data, per OS user).
        Log("no device id on this machine - creating one");
        createdDevice = true;
        EOS_Connect_CreateDeviceIdOptions options = {};
        options.ApiVersion = EOS_CONNECT_CREATEDEVICEID_API_LATEST;
#if defined(_WIN32)
        options.DeviceModel = "PC Windows";
#else
        options.DeviceModel = "PC Linux";
#endif
        EOS_Connect_CreateDeviceId(Connect(), &options, nullptr, &EosLoginFlow::OnCreateDeviceId);
    } else if (data->ResultCode == EOS_EResult::EOS_InvalidUser && data->ContinuanceToken != nullptr) {
        // The device id is known but no EOS user exists for this product yet: first login.
        Log("first login with this device - creating the EOS user");
        EOS_Connect_CreateUserOptions options = {};
        options.ApiVersion = EOS_CONNECT_CREATEUSER_API_LATEST;
        options.ContinuanceToken = data->ContinuanceToken;
        EOS_Connect_CreateUser(Connect(), &options, nullptr, &EosLoginFlow::OnCreateUser);
    } else {
        SetFailed("Connect login", data->ResultCode);
    }
}

void EosLoginFlow::OnCreateDeviceId (const EOS_Connect_CreateDeviceIdCallbackInfo* data) {
    // DuplicateNotAllowed = a device id already exists, which is just as good.
    if (data->ResultCode == EOS_EResult::EOS_Success || data->ResultCode == EOS_EResult::EOS_DuplicateNotAllowed) {
        Login(EosPlatform::Get());
    } else {
        SetFailed("create device id", data->ResultCode);
    }
}

void EosLoginFlow::OnCreateUser (const EOS_Connect_CreateUserCallbackInfo* data) {
    if (data->ResultCode == EOS_EResult::EOS_Success) {
        SetLoggedIn(data->LocalUserId);
    } else {
        SetFailed("create user", data->ResultCode);
    }
}

#endif // SIMTARY_HAS_EOS

void EosPlatform::Initialize (const EosConfig& config, const std::string& productName, const std::string& productVersion) {
    config_ = config;
    config_.clientId     = Override("--eos-client-id=", "EOS_CLIENT_ID", config.clientId);
    config_.clientSecret = Override("--eos-client-secret=", "EOS_CLIENT_SECRET", config.clientSecret);
    config_.sandboxId    = Override("--eos-sandbox=", nullptr, config.sandboxId);
    config_.deploymentId = Override("--eos-deployment=", nullptr, config.deploymentId);

    status_ = {};
    status_.server = st::IsHeadless();
    RegisterBridge();

#if !defined(SIMTARY_HAS_EOS)
    (void)productName; (void)productVersion;
    if (!config_.productId.empty()) Log("this build has no EOS SDK (deps/eos-sdk) - online services off");
    return;
#else
    status_.compiled = true;
    if (wi::arguments::HasArgument("--no-eos")) { Log("off (--no-eos)"); return; }
    if (config_.productId.empty()) return;   // the project does not use EOS
    if (config_.sandboxId.empty() || config_.deploymentId.empty() || config_.clientId.empty() || config_.clientSecret.empty()) {
        status_.state = EosState::Failed;
        status_.error = "incomplete configuration (sandbox, deployment, client id and client secret are all needed)";
        Log(status_.error, wi::backlog::LogLevel::Error);
        return;
    }

    EOS_InitializeOptions init = {};
    init.ApiVersion = EOS_INITIALIZE_API_LATEST;
    init.ProductName = productName.c_str();
    init.ProductVersion = productVersion.c_str();
    EOS_EResult result = EOS_Initialize(&init);
    if (result != EOS_EResult::EOS_Success && result != EOS_EResult::EOS_AlreadyConfigured) {
        status_.state = EosState::Failed;
        status_.error = "EOS_Initialize: " + ResultName(result);
        Log(status_.error, wi::backlog::LogLevel::Error);
        return;
    }
    sdkInitialized_ = true;
    EOS_Logging_SetCallback(&OnLogMessage);
    EOS_Logging_SetLogLevel(EOS_ELogCategory::EOS_LC_ALL_CATEGORIES, EOS_ELogLevel::EOS_LOG_Warning);

    // The SDK keeps its cache (device id, tokens) here. Absolute path required.
    std::error_code ec;
    const std::filesystem::path cache = std::filesystem::absolute(st::userdata::BaseDir() + "eos", ec);
    std::filesystem::create_directories(cache, ec);
    const std::string cacheDir = cache.string();

    EOS_Platform_Options options = {};
    options.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
    options.ProductId = config_.productId.c_str();
    options.SandboxId = config_.sandboxId.c_str();
    options.DeploymentId = config_.deploymentId.c_str();
    options.ClientCredentials.ClientId = config_.clientId.c_str();
    options.ClientCredentials.ClientSecret = config_.clientSecret.c_str();
    options.bIsServer = status_.server ? EOS_TRUE : EOS_FALSE;
    // No social overlay yet: it hooks the swapchain, which needs its own integration.
    options.Flags = EOS_PF_DISABLE_OVERLAY;
    options.CacheDirectory = cacheDir.c_str();
    options.TickBudgetInMilliseconds = 0;   // no budget: finish the work each tick
    platform_ = EOS_Platform_Create(&options);
    if (platform_ == nullptr) {
        status_.state = EosState::Failed;
        status_.error = "EOS_Platform_Create failed (check the product / sandbox / deployment / client ids)";
        Log(status_.error, wi::backlog::LogLevel::Error);
        return;
    }

    status_.state = EosState::Ready;
    Log(std::string("platform up as ") + (status_.server ? "server" : "client") + ", sandbox " + config_.sandboxId);

    std::string& name = EosLoginFlow::displayName;
    name = config_.displayName.empty() ? productName : config_.displayName;
    if (name.size() > EOS_CONNECT_USERLOGININFO_DISPLAYNAME_MAX_LENGTH)
        name.resize(EOS_CONNECT_USERLOGININFO_DISPLAYNAME_MAX_LENGTH);
    if (!status_.server && config_.autoLogin) LoginDevice();
#endif
}

void EosPlatform::LoginDevice () {
#if defined(SIMTARY_HAS_EOS)
    if (platform_ == nullptr || status_.server) return;
    if (status_.state == EosState::LoggingIn || status_.state == EosState::LoggedIn) return;
    status_.state = EosState::LoggingIn;
    EosLoginFlow::createdDevice = false;
    EosLoginFlow::Login(*this);
#endif
}

void EosPlatform::Update () {
#if defined(SIMTARY_HAS_EOS)
    if (platform_ != nullptr) EOS_Platform_Tick(EOS_HPlatform(platform_));
#endif
}

void EosPlatform::Shutdown () {
#if defined(SIMTARY_HAS_EOS)
    if (platform_ != nullptr) {
        EOS_Platform_Release(EOS_HPlatform(platform_));
        platform_ = nullptr;
    }
    // EOS_Shutdown is final for the process - the SDK cannot be initialized again after it.
    if (sdkInitialized_) {
        EOS_Shutdown();
        sdkInitialized_ = false;
    }
#endif
    if (status_.state != EosState::Failed) status_.state = EosState::Off;
}

void EosPlatform::RegisterBridge () {
    if (!st::dev::IsDevBuild()) return;
    st::dev::EditorBridge::Get().Register("eos.status", "{} -> Epic Online Services: compiled, state, ProductUserId, last error",
        [this](const st::json::Value&, std::string&) {
            st::json::Value o = st::json::Value::Object();
            o.Set("compiled", status_.compiled);
            o.Set("server", status_.server);
            o.Set("state", StateName(status_.state));
            o.Set("productUserId", status_.productUserId);
            o.Set("error", status_.error);
            o.Set("sandboxId", config_.sandboxId);
            return o;
        });
    st::dev::EditorBridge::Get().Register("eos.login", "{} -> start the Device ID login (client only)",
        [this](const st::json::Value&, std::string&) {
            LoginDevice();
            st::json::Value o = st::json::Value::Object();
            o.Set("state", StateName(status_.state));
            return o;
        });
}

} // namespace st::online
