// The "stNetPlayerSpawn" native component: one player object per player (MULTIPLAYER.md v2,
// step 5). Put it on an entity in the map - where it stands is where players appear, side by
// side along its right axis:
//
//     NCI_0              = "stNetPlayerSpawn"
//     NCA_0_template     = "assets/templates/Player.sttd"
//     NCA_0_spacing      = 3.0
//     NCA_0_localOnly    = "stInput,stChaseCamera,stMainCamera,stCharacter,stVehicleHud"
//
// On the server (dedicated or host) every player that joins gets a copy of the template, owned
// by that player with transform authority: the player's own machine moves it, everyone else
// sees it replicated. `localOnly` components run only on the owner's machine. The copy goes
// when the player leaves. Offline nothing is spawned: a single-player map keeps its own player.
#include "net/Replication.h"
#include "net/NetSession.h"

#include "stNativeComponent.h"
#include "wiScene.h"
#include "wiBacklog.h"
#include "imgui.h"

#include <unordered_map>

namespace st::net {

struct NetPlayerSpawnComponent : wi::scene::NativeComponent {
    std::string templatePath;
    float spacing = 3.0f;
    std::string localOnly = "stInput,stChaseCamera,stMainCamera,stCharacter,stVehicleHud";

    int listener_ = 0;
    uint32_t nextSlot_ = 0;
    std::unordered_map<uint32_t, wi::ecs::Entity> players_;   // client id -> its object

    void Start () override {
        Bind(templatePath, "template");
        Bind(spacing, "spacing");
        Bind(localOnly, "localOnly");
        NetSession& net = NetSession::Get();
        if (!net.IsServer()) return;
        listener_ = net.AddPlayerListener([this](uint32_t clientId, bool joined) {
            if (joined) SpawnFor(clientId);
            else DespawnFor(clientId);
        });
        // Players that were in before this map started (a host's own player, a reload).
        if (net.HasLocalPlayer()) SpawnFor(net.LocalClientId());
        for (const Peer& p : net.Peers())
            if (p.clientId != 0) SpawnFor(p.clientId);
    }

    void Destroy () override {
        if (listener_ != 0) NetSession::Get().RemovePlayerListener(listener_);
        listener_ = 0;
        players_.clear();
    }

    void SpawnFor (uint32_t clientId) {
        if (templatePath.empty() || players_.count(clientId)) return;
        XMFLOAT3 pos(0, 0, 0), right(1, 0, 0);
        XMFLOAT4 rot(0, 0, 0, 1);
        if (const wi::scene::TransformComponent* t = scene->transforms.GetComponent(entity)) {
            pos = t->GetPosition();
            rot = t->GetRotation();
            right = t->GetRight();
        }
        const float offset = spacing * float(nextSlot_++);
        pos.x += right.x * offset;
        pos.y += right.y * offset;
        pos.z += right.z * offset;
        Replication::Options options;
        options.syncTransform = true;
        options.ownerAuthority = true;
        options.localOnly = localOnly;
        const wi::ecs::Entity e = Replication::Get().Spawn(templatePath, pos, rot, clientId, &options);
        if (e == wi::ecs::INVALID_ENTITY) return;
        players_[clientId] = e;
        wi::backlog::post("[net] player " + std::to_string(clientId) + " spawned from " + templatePath);
    }

    void DespawnFor (uint32_t clientId) {
        auto it = players_.find(clientId);
        if (it == players_.end()) return;
        Replication::Get().Despawn(it->second);
        players_.erase(it);
    }

    void DescribeParams (wi::vector<NativeParam>& out) override {
        out.push_back(NativeParam::Asset("template", &templatePath, "Template (.sttd) every joining player gets a copy of."));
        out.push_back(NativeParam::Float("spacing", &spacing, 0.0f, 50.0f, "Metres between players along this entity's right axis."));
        out.push_back(NativeParam::String("localOnly", &localOnly,
            "Components that run only on the owning player's machine, comma separated."));
    }
};

} // namespace st::net

using StNetPlayerSpawn = st::net::NetPlayerSpawnComponent;
ST_REGISTER_FRAMEWORK_COMPONENT_AS(StNetPlayerSpawn, "stNetPlayerSpawn", "Network")
