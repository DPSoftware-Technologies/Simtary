// The "stNetObject" native component: this entity is replicated (net/Replication.h,
// MULTIPLAYER.md v2). Attach it in the editor to anything that has to look the same on every
// machine - a door, a crate, a moving platform - or put it on a template's root for objects
// the server spawns.
//
//     NCI_0                = "stNetObject"
//     NCA_0_syncTransform  = true    the server's transform reaches every client
//     NCA_0_ownerAuthority = false   the owning client moves it (characters, vehicles)
//
// The editor stamps the entity's EntityGUID when the component is inspected, which is how the
// same placed object is found on every machine; without one the name path is used.
#include "net/Replication.h"
#include "net/NetSession.h"

#include "stNativeComponent.h"
#include "wiScene.h"
#include "imgui.h"

namespace st::net {

struct NetObjectComponent : wi::scene::NativeComponent {
    bool syncTransform = true;
    bool ownerAuthority = false;

    void Start () override {
        Bind(syncTransform, "syncTransform");
        Bind(ownerAuthority, "ownerAuthority");
        Replication::Get().Register(entity, { syncTransform, ownerAuthority });
    }

    void Destroy () override { Replication::Get().Unregister(entity); }

    void DrawDebug () override {
        // A placed object is matched across machines by its GUID: make sure it has one, and
        //	that it is saved with the map.
        if (scene != nullptr) wi::scene::EnsureEntityGUID(*scene, entity);

        bool dirty = false;
        dirty |= ImGui::Checkbox("Sync transform", &syncTransform);
        dirty |= ImGui::Checkbox("Owner authority", &ownerAuthority);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The owning client moves this object itself (a player's character or vehicle);\n"
                              "the server applies what it sends and passes it on.");
        if (dirty) {
            SaveBoundParams();
            Replication& r = Replication::Get();
            if (r.NetId(entity) != 0 || NetSession::Get().IsOnline())
                r.Register(entity, { syncTransform, ownerAuthority });   // updates the options
        }

        const NetSession& net = NetSession::Get();
        const Replication& r = Replication::Get();
        if (!net.IsOnline()) {
            ImGui::TextDisabled("offline - replicated once a session runs");
            return;
        }
        ImGui::Text("net id %u   owner %s", r.NetId(entity),
                    r.OwnerOf(entity) == 0 ? "server" : ("client " + std::to_string(r.OwnerOf(entity))).c_str());
        ImGui::TextDisabled(r.HasAuthority(entity) ? "driven here" : "driven remotely (proxy here)");
    }
};

} // namespace st::net

using StNetObject = st::net::NetObjectComponent;
ST_REGISTER_FRAMEWORK_COMPONENT_AS(StNetObject, "stNetObject", "Network")
