#include "net/NetUI.h"

#include "io/PlayerPrefs.h"
#include "net/NetSession.h"

#include "imgui.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace st::net {

namespace {

struct Form {
    char name[48] = {};
    char address[96] = {};
    int port = int(kDefaultPort);
    int transport = 0;          // 0 UDP (GNS), 1 TCP
    bool openToNetwork = true;
    bool loaded = false;
};

Form g_form;

void LoadForm () {
    if (g_form.loaded) return;
    g_form.loaded = true;
    const std::string name = st::PlayerPrefs::GetString("net.name", "Player");
    const std::string address = st::PlayerPrefs::GetString("net.address", "127.0.0.1:" + std::to_string(kDefaultPort));
    snprintf(g_form.name, sizeof(g_form.name), "%s", name.c_str());
    snprintf(g_form.address, sizeof(g_form.address), "%s", address.c_str());
    g_form.port = st::PlayerPrefs::GetInt("net.port", int(kDefaultPort));
    g_form.transport = st::PlayerPrefs::GetInt("net.transport", 0);
    g_form.openToNetwork = st::PlayerPrefs::GetBool("net.open", true);
}

void SaveForm () {
    st::PlayerPrefs::SetString("net.name", g_form.name);
    st::PlayerPrefs::SetString("net.address", g_form.address);
    st::PlayerPrefs::SetInt("net.port", g_form.port);
    st::PlayerPrefs::SetInt("net.transport", g_form.transport);
    st::PlayerPrefs::SetBool("net.open", g_form.openToNetwork);
    st::PlayerPrefs::Save();
}

const char* TransportName () { return g_form.transport == 1 ? "tcp" : "gns"; }

} // namespace

void DrawMultiplayerWindow (bool& open, const std::function<void()>& reloadScene) {
    if (!open) return;
    LoadForm();
    NetSession& net = NetSession::Get();

    ImGui::SetNextWindowPos(ImVec2(20, 60), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Multiplayer (F10)", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    // ---- status ----
    switch (net.GetMode()) {
        case Mode::Off:
            ImGui::TextDisabled("Offline (single player)");
            break;
        case Mode::Host:
            ImGui::Text("Hosting \"%s\"", net.Config().name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled(net.IsListening() ? "- open on port %d" : "- not open to the network", int(net.Config().port));
            break;
        case Mode::Server:
            ImGui::Text("Dedicated server on port %d", int(net.Config().port));
            break;
        case Mode::Client:
            if (net.IsReady()) {
                int ping = -1;
                if (!net.Peers().empty() && net.GetTransport() != nullptr) ping = net.GetTransport()->Stats(net.Peers()[0].connection).pingMs;
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "Connected to %s", net.ConnectAddress().c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("ping %d ms, you are #%u", ping, net.LocalClientId());
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Connecting to %s ...", net.ConnectAddress().c_str());
            }
            break;
    }
    if (!net.LastError().empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", net.LastError().c_str());
    ImGui::Separator();

    const bool offline = net.GetMode() == Mode::Off;
    const char* transports[] = { "UDP (GameNetworkingSockets)", "TCP" };

    if (offline) {
        ImGui::InputText("Your name", g_form.name, sizeof(g_form.name));
        ImGui::Combo("Connection", &g_form.transport, transports, 2);

        ImGui::SeparatorText("Join a game");
        ImGui::InputText("Address", g_form.address, sizeof(g_form.address));
        ImGui::TextDisabled("host:port - the server must run this scene");
        if (ImGui::Button("Join")) {
            SaveForm();
            net.StartClient(g_form.address, g_form.name, TransportName());
        }

        ImGui::SeparatorText("Host a game");
        ImGui::InputInt("Port", &g_form.port);
        ImGui::Checkbox("Open to other players", &g_form.openToNetwork);
        if (ImGui::Button("Host")) {
            SaveForm();
            g_form.port = g_form.port < 1 ? 1 : (g_form.port > 65535 ? 65535 : g_form.port);
            net.SetPlayerName(g_form.name);
            if (net.StartHost() && g_form.openToNetwork) net.OpenToLan(uint16_t(g_form.port), TransportName());
        }
    } else {
        if (net.IsServer()) {
            if (net.GetMode() == Mode::Host) {
                if (!net.IsListening()) {
                    ImGui::InputInt("Port", &g_form.port);
                    if (ImGui::Button("Open to the network")) {
                        g_form.port = g_form.port < 1 ? 1 : (g_form.port > 65535 ? 65535 : g_form.port);
                        SaveForm();
                        net.OpenToLan(uint16_t(g_form.port), TransportName());
                    }
                } else if (ImGui::Button("Close to the network")) {
                    net.CloseLan();
                }
            }
            ImGui::SeparatorText("Players");
            ImGui::Text("#%u %s (you)", net.LocalClientId(), net.HasLocalPlayer() ? net.PlayerName().c_str() : "server");
            uint32_t kick = 0;
            for (const Peer& p : net.Peers()) {
                if (p.clientId == 0) continue;
                const int ping = net.GetTransport() != nullptr ? net.GetTransport()->Stats(p.connection).pingMs : -1;
                ImGui::PushID(int(p.clientId));
                ImGui::Text("#%u %s  %d ms", p.clientId, p.name.c_str(), ping);
                ImGui::SameLine();
                if (ImGui::SmallButton("Kick")) kick = p.clientId;
                ImGui::PopID();
            }
            if (kick != 0) net.Kick(kick, "kicked by the host");
            ImGui::Separator();
        }
        if (ImGui::Button(net.GetMode() == Mode::Client ? "Leave" : "Stop")) {
            net.Leave();
            if (reloadScene) reloadScene();
        }
    }
    ImGui::End();
}

} // namespace st::net
