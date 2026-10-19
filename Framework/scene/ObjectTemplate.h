#pragma once
// ObjectTemplate - one object, saved on its own so any scene can place copies of it.
//
// A template is an entity, everything under it in the Hierarchy, and every entity those
// reference that does not live under it: the mesh an object draws, the materials that mesh
// uses, the armature a skin is bound to, the animation data a channel reads. Saved, it is a
// .sttd ("Simtary Template Descriptor") - the SAME container a .stsd map uses (NBT header +
// a .wiscene blob with its resource block emptied), marked as a template by a "template"
// compound under the descriptor's `project` tag. So it shares the map's whole resource
// story: textures and meshes are referenced by name and live in the asset package (or in
// the project's assets/resources/ for the next build), never copied into the file.
//
// What the save normalises, so a template is the same thing whichever scene it came from:
//   - the root sits at the world origin with no rotation; its world SCALE is kept, because
//     scale is part of how the object looks while position and heading are where it was
//     put. Placement comes from whoever instantiates it.
//   - every pulled-in dependency is parented under the root, so a placed copy is ONE row
//     in the Hierarchy and one thing to move, undo or delete.
//   - native-component entity references (NCA_ strings holding an EntityGUID) that point
//     OUTSIDE the template are cleared and counted: kept, they would resolve to whatever
//     entity happens to carry that GUID in the scene the copy lands in.
//
//     st::ObjectTemplate tpl;
//     if (tpl.Load("assets/templates/soldier.sttd"))
//         tpl.Instantiate(wi::scene::GetScene(), XMFLOAT3(10, 0, 4));
//
// Every copy gets fresh entity ids AND fresh EntityGUIDs, with the references between its
// own entities rewritten to match - ten soldiers each aim their own rifle, not the first one's.
//
// No exceptions, no RTTI.

#include "wiScene.h"
#include "io/Nbt.h"

#include <memory>
#include <string>
#include <vector>

namespace st {

// The file extension, lowercase, no dot.
inline constexpr const char* kTemplateExtension = "sttd";

// Where the editor saves templates, relative to the game's working directory. A dev build
// mirrors it into <project>/assets/contents/templates/, so the next build packs it.
inline constexpr const char* kTemplateFolder = "assets/templates";

bool IsTemplatePath (const std::string& path);

struct TemplateBuildReport {
    uint32_t entities     = 0;  // everything that went into the template
    uint32_t subtree      = 0;  // the root and its descendants
    uint32_t dependencies = 0;  // pulled in from outside the subtree (meshes, materials, ...)
    uint32_t clearedRefs  = 0;  // entity references to the outside, cleared
    std::string error;          // set when the build returned INVALID_ENTITY
};

// Copy `root`, its descendants and everything they reference out of `source` into `out`
// (expected empty), normalised as described above. Entity ids are kept, so the root in
// `out` is `root`. Returns INVALID_ENTITY when `root` has no transform.
wi::ecs::Entity BuildTemplateScene (wi::scene::Scene& source, wi::ecs::Entity root,
                                    wi::scene::Scene& out, TemplateBuildReport* report = nullptr);

// The tag a template's descriptor carries under `project`. Presence of the "template" key
// there is what makes a descriptor a template.
nbt::Tag TemplateProjectTag (const wi::scene::Scene& tmpl, wi::ecs::Entity root);
bool     IsTemplateProjectTag (const nbt::Tag& project);

// The template's one top-level transform. INVALID_ENTITY in an empty scene.
wi::ecs::Entity FindTemplateRoot (const wi::scene::Scene& tmpl);

class ObjectTemplate {
public:
    ObjectTemplate ();
    ~ObjectTemplate ();
    ObjectTemplate (const ObjectTemplate&) = delete;
    ObjectTemplate& operator= (const ObjectTemplate&) = delete;

    // Read and deserialize a .sttd. `path` may be a disk path or a package name - the
    // loose file wins when both exist, so a template re-saved this session is not shadowed
    // by the copy packed at launch.
    bool Load (const std::string& path, std::string* error = nullptr);
    void Unload ();

    bool IsLoaded () const { return prefab_ != nullptr; }
    const std::string& Path () const { return path_; }      // what Load() actually opened
    const std::string& Name () const { return name_; }      // the root's name

    // The loaded contents, root at the origin. Instantiate() copies out of this; the
    // template preview edits it directly.
    wi::scene::Scene&       Prefab ()       { return *prefab_; }
    const wi::scene::Scene& Prefab () const { return *prefab_; }
    wi::ecs::Entity         Root   () const { return root_; }

    // Copy the template into `target` with its root at `position` / `rotation`. Returns
    // the new root; `created` (optional) receives every entity the copy made, root first.
    wi::ecs::Entity Instantiate (wi::scene::Scene& target, const XMFLOAT3& position,
                                 const XMFLOAT4& rotation = XMFLOAT4(0, 0, 0, 1),
                                 wi::vector<wi::ecs::Entity>* created = nullptr);

    // Hand the loaded scene over (the template preview keeps it as its own). Leaves this
    // object unloaded.
    std::unique_ptr<wi::scene::Scene> Release ();

private:
    std::unique_ptr<wi::scene::Scene> prefab_;
    wi::ecs::Entity root_ = wi::ecs::INVALID_ENTITY;
    std::string     path_;
    std::string     name_;
    // The prefab's component managers serialized once, then replayed for every copy - the
    // same trick wi::scene::Scene::Instantiate plays with its optimized_instatiation_data.
    wi::Archive     instanceData_;
    bool            instanceDataReady_ = false;
};

// Read a template's bytes: the loose file first (disk path, or under assets/), then the
// mounted packages. `resolved` gets the path that answered.
bool ReadTemplateFile (const std::string& path, std::vector<uint8_t>& out,
                       std::string* resolved = nullptr, std::string* error = nullptr);

} // namespace st
