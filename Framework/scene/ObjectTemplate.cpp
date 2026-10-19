#include "scene/ObjectTemplate.h"
#include "io/asset/AssetSystem.h"
#include "io/asset/SceneDescriptor.h"
#include "io/asset/StHash.h"
#include "stNativeComponent.h"
#include "wiHelper.h"
#include "wiBacklog.h"

#include <filesystem>
#include <unordered_map>
#include <unordered_set>

using wi::ecs::Entity;
using wi::ecs::INVALID_ENTITY;
using namespace wi::scene;

namespace st {

namespace {

constexpr const char* kGuidKey = "EntityGUID"; // stNativeComponent.cpp's kEntityGUIDKey

bool HasAnyComponent (const Scene& scene, Entity e) {
    for (const auto& kv : scene.componentLibrary.entries) {
        const auto* mgr = kv.second.component_manager.get();
        if (mgr != nullptr && mgr->Contains(e)) return true;
    }
    return false;
}

// Every entity `e`'s components point at. Only the references that make the object WORK
// are followed - what it draws, what deforms it, what animates it, what it is jointed to.
void ForwardReferences (const Scene& scene, Entity e, std::vector<Entity>& out) {
    if (const ObjectComponent* o = scene.objects.GetComponent(e))
        out.push_back(o->meshID);
    if (const MeshComponent* m = scene.meshes.GetComponent(e)) {
        out.push_back(m->armatureID);
        for (const auto& subset : m->subsets) out.push_back(subset.materialID);
    }
    if (const ArmatureComponent* a = scene.armatures.GetComponent(e))
        out.insert(out.end(), a->boneCollection.begin(), a->boneCollection.end());
    if (const AnimationComponent* a = scene.animations.GetComponent(e)) {
        for (const auto& c : a->channels) out.push_back(c.target);
        for (const auto& s : a->samplers) out.push_back(s.data);
        for (const auto& r : a->retargets) out.push_back(r.source);
    }
    if (const InverseKinematicsComponent* ik = scene.inverse_kinematics.GetComponent(e))
        out.push_back(ik->target);
    if (const HumanoidComponent* h = scene.humanoids.GetComponent(e))
        for (Entity bone : h->bones) out.push_back(bone);
    if (const ExpressionComponent* x = scene.expressions.GetComponent(e))
        for (const auto& expr : x->expressions)
            for (const auto& b : expr.morph_target_bindings) out.push_back(b.meshID);
    if (const PhysicsConstraintComponent* c = scene.constraints.GetComponent(e)) {
        out.push_back(c->bodyA);
        out.push_back(c->bodyB);
    }
    if (const SplineComponent* s = scene.splines.GetComponent(e))
        out.insert(out.end(), s->spline_node_entities.begin(), s->spline_node_entities.end());
    if (const wi::EmittedParticleSystem* p = scene.emitters.GetComponent(e))
        out.push_back(p->meshID);
    if (const wi::HairParticleSystem* p = scene.hairs.GetComponent(e))
        out.push_back(p->meshID);
}

bool StartsWith (const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

} // namespace

bool IsTemplatePath (const std::string& path) {
    return wi::helper::toLower(wi::helper::GetExtensionFromFileName(path)) == kTemplateExtension;
}

Entity FindTemplateRoot (const Scene& tmpl) {
    // Built templates have exactly one top-level transform. A hand-edited file with more
    // than one still loads: the first one wins, and the rest are its siblings in the file
    // but never reachable from a placed copy, so they are reported.
    Entity root = INVALID_ENTITY;
    uint32_t tops = 0;
    for (size_t i = 0; i < tmpl.transforms.GetCount(); ++i) {
        const Entity e = tmpl.transforms.GetEntity(i);
        if (tmpl.hierarchy.Contains(e)) continue;
        if (root == INVALID_ENTITY) root = e;
        ++tops;
    }
    if (tops > 1)
        wi::backlog::post("Template: " + std::to_string(tops) + " top-level objects; using the first",
                          wi::backlog::LogLevel::Warning);
    return root;
}

Entity BuildTemplateScene (Scene& source, Entity root, Scene& out, TemplateBuildReport* report) {
    TemplateBuildReport local;
    TemplateBuildReport& r = report ? *report : local;
    r = TemplateBuildReport{};

    if (root == INVALID_ENTITY || !source.transforms.Contains(root)) {
        r.error = "a template root needs a transform";
        return INVALID_ENTITY;
    }

    // The subtree, in hierarchy order so a parent is always written before its children.
    std::unordered_set<Entity> keep;
    std::vector<Entity> order;
    keep.insert(root);
    order.push_back(root);
    for (size_t i = 0; i < source.hierarchy.GetCount(); ++i) {
        const Entity e = source.hierarchy.GetEntity(i);
        if (source.Entity_IsDescendant(e, root) && keep.insert(e).second)
            order.push_back(e);
    }
    r.subtree = (uint32_t)order.size();

    // Dependency closure. Forward references until nothing new appears, then the one
    // reverse reference worth following: an animation that drives anything in the set
    // belongs to the object even when it sits outside it (a glTF import leaves its clips
    // as loose top-level entities). Repeat, since a pulled-in clip brings its data.
    std::vector<Entity> work(order.begin(), order.end());
    std::vector<Entity> refs;
    for (;;) {
        while (!work.empty()) {
            const Entity e = work.back();
            work.pop_back();
            refs.clear();
            ForwardReferences(source, e, refs);
            for (Entity ref : refs) {
                if (ref == INVALID_ENTITY || keep.count(ref) || !HasAnyComponent(source, ref)) continue;
                keep.insert(ref);
                order.push_back(ref);
                work.push_back(ref);
            }
        }
        for (size_t i = 0; i < source.animations.GetCount(); ++i) {
            const Entity e = source.animations.GetEntity(i);
            if (keep.count(e)) continue;
            for (const auto& c : source.animations[i].channels) {
                if (keep.count(c.target)) {
                    keep.insert(e);
                    order.push_back(e);
                    work.push_back(e);
                    break;
                }
            }
        }
        if (work.empty()) break;
    }
    r.entities     = (uint32_t)order.size();
    r.dependencies = r.entities - r.subtree;

    // Copy with the ids kept, so every reference inside the set stays valid as it is.
    {
        wi::Archive archive;
        archive.SetReadModeAndResetPos(false);
        wi::ecs::EntitySerializer seri;
        seri.allow_remap = false;
        for (Entity e : order)
            source.Entity_Serialize(archive, seri, e, Scene::EntitySerializeFlags::KEEP_INTERNAL_ENTITY_REFERENCES);
        archive.SetReadModeAndResetPos(true);
        wi::ecs::EntitySerializer reader;
        reader.allow_remap = false;
        for (size_t i = 0; i < order.size(); ++i)
            out.Entity_Serialize(archive, reader, INVALID_ENTITY, Scene::EntitySerializeFlags::KEEP_INTERNAL_ENTITY_REFERENCES);
    }

    // The root: at the origin, unrotated, at its world scale, and parentless.
    const XMMATRIX rootWorld = source.transforms.GetComponent(root)->GetWorldMatrix();
    XMVECTOR rootScale, rootRotation, rootTranslation;
    XMMatrixDecompose(&rootScale, &rootRotation, &rootTranslation, rootWorld);
    out.hierarchy.Remove(root);
    if (TransformComponent* t = out.transforms.GetComponent(root)) {
        XMStoreFloat3(&t->scale_local, rootScale);
        t->rotation_local = XMFLOAT4(0, 0, 0, 1);
        t->SetWorldPosition(0.0, 0.0, 0.0);
        t->RebaseToOrigin(wi::scene::GetRenderOrigin());
        t->SetDirty();
        t->UpdateTransform();
    }
    // Where the root sat, without its scale: what a pulled-in transform is re-expressed
    // against, so a bone that lived elsewhere keeps its place relative to the object.
    const XMMATRIX rootFrameInverse = XMMatrixInverse(nullptr,
        XMMatrixRotationQuaternion(rootRotation) * XMMatrixTranslationFromVector(rootTranslation));

    // Everything else that is not already a descendant goes under the root.
    for (size_t i = 1; i < order.size(); ++i) {
        const Entity e = order[i];
        if (const HierarchyComponent* h = out.hierarchy.GetComponent(e)) {
            if (keep.count(h->parentID)) continue; // part of the tree already
            out.hierarchy.Remove(e);
        }
        if (TransformComponent* t = out.transforms.GetComponent(e)) {
            const TransformComponent* src = source.transforms.GetComponent(e);
            const XMMATRIX world = src->GetWorldMatrix() * rootFrameInverse;
            XMVECTOR s, q, p;
            XMMatrixDecompose(&s, &q, &p, world);
            XMStoreFloat3(&t->scale_local, s);
            XMStoreFloat4(&t->rotation_local, q);
            XMStoreFloat3(&t->translation_local, p);
            t->SyncWorldFromLocal(wi::scene::GetRenderOrigin());
            t->SetDirty();
        }
        out.Component_Attach(e, root, true);
    }

    // Native-component references that leave the template. A GUID string in an NCA_ arg
    // that names an entity of the SOURCE scene which did not come along would, in the
    // scene a copy lands in, name whatever entity carries that GUID there.
    std::unordered_set<std::string> outsideGuids;
    for (size_t i = 0; i < source.metadatas.GetCount(); ++i) {
        const Entity e = source.metadatas.GetEntity(i);
        if (keep.count(e)) continue;
        const MetadataComponent& m = source.metadatas[i];
        if (m.string_values.has(kGuidKey)) outsideGuids.insert(m.string_values.get(kGuidKey));
    }
    if (!outsideGuids.empty()) {
        for (size_t i = 0; i < out.metadatas.GetCount(); ++i) {
            MetadataComponent& m = out.metadatas[i];
            for (size_t v = 0; v < m.string_values.values.size(); ++v) {
                if (!StartsWith(m.string_values.names[v], "NCA_")) continue;
                if (outsideGuids.count(m.string_values.values[v]) == 0) continue;
                m.string_values.values[v].clear();
                ++r.clearedRefs;
            }
        }
    }

    return root;
}

nbt::Tag TemplateProjectTag (const Scene& tmpl, Entity root) {
    nbt::Tag info = nbt::Tag::Compound();
    info.putInt("version", 1);
    const NameComponent* name = tmpl.names.GetComponent(root);
    info.putString("root", name ? name->name : std::string());
    nbt::Tag project = nbt::Tag::Compound();
    project.put("template", std::move(info));
    return project;
}

bool IsTemplateProjectTag (const nbt::Tag& project) {
    const nbt::Tag* t = project.get("template");
    return t != nullptr && t->isCompound();
}

bool ReadTemplateFile (const std::string& path, std::vector<uint8_t>& out,
                       std::string* resolved, std::string* error) {
    // A package name ("templates/x.sttd") is also tried under assets/, which is where the
    // editor writes the loose copy.
    std::vector<std::string> candidates{ path };
    const std::string normalized = asset::NormalizePath(path);
    if (!StartsWith(wi::helper::toLower(normalized), "assets/"))
        candidates.push_back("assets/" + normalized);

    // Loose file first: the package was built at launch, so after a re-save it holds the
    // OLD bytes, and a mounted package otherwise shadows the disk.
    for (const std::string& c : candidates) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(std::filesystem::u8path(c), ec)) continue;
        wi::vector<uint8_t> bytes;
        if (!wi::helper::FileRead(c, bytes)) continue;
        out.assign(bytes.begin(), bytes.end());
        if (resolved) *resolved = c;
        return true;
    }
    for (const std::string& c : candidates) {
        if (AssetSystem::Get().Read(c, out, nullptr)) {
            if (resolved) *resolved = c;
            return true;
        }
    }
    if (error) *error = "no template at " + path;
    return false;
}

// ObjectTemplate

ObjectTemplate::ObjectTemplate () = default;
ObjectTemplate::~ObjectTemplate () = default;

void ObjectTemplate::Unload () {
    prefab_.reset();
    root_ = INVALID_ENTITY;
    path_.clear();
    name_.clear();
    instanceData_ = wi::Archive();
    instanceDataReady_ = false;
}

bool ObjectTemplate::Load (const std::string& path, std::string* error) {
    Unload();

    std::vector<uint8_t> bytes;
    std::string resolved;
    if (!ReadTemplateFile(path, bytes, &resolved, error)) return false;

    asset::SceneDescriptor descriptor;
    if (!asset::ParseSceneDescriptor(bytes.data(), bytes.size(), descriptor, true, error)) return false;
    if (!IsTemplateProjectTag(descriptor.project)) {
        if (error) *error = resolved + " is a map, not a template";
        return false;
    }
    const std::vector<uint8_t>* ecs = descriptor.EcsArchive();
    if (ecs == nullptr || ecs->empty()) {
        if (error) *error = resolved + " has no entity payload";
        return false;
    }

    const std::vector<std::string> missing = AssetSystem::Get().MissingAssetsFor(descriptor);
    if (!missing.empty())
        wi::backlog::post("Template: " + resolved + " is missing " + std::to_string(missing.size()) +
                          " of " + std::to_string(descriptor.assets.size()) + " assets (first: " +
                          missing.front() + ")", wi::backlog::LogLevel::Warning);

    auto scene = std::make_unique<Scene>();
    {
        // A memory archive has no source directory, which keeps the resource names inside
        // it relative - how the package stores them (same as AssetSystem::LoadScene).
        wi::Archive archive(ecs->data(), ecs->size());
        if (!archive.IsOpen()) {
            if (error) *error = resolved + ": stored archive is not readable by this engine build";
            return false;
        }
        scene->Serialize(archive);
    }
    // Instantiated copies are run by the scene they land in; the prefab itself never is.
    scene->nativeComponents.lifecycleEnabled = false;

    const Entity root = FindTemplateRoot(*scene);
    if (root == INVALID_ENTITY) {
        if (error) *error = resolved + " is empty";
        return false;
    }

    prefab_ = std::move(scene);
    root_   = root;
    path_   = resolved;
    if (const NameComponent* n = prefab_->names.GetComponent(root_)) name_ = n->name;
    if (name_.empty()) name_ = wi::helper::RemoveExtension(wi::helper::GetFileNameFromPath(resolved));
    return true;
}

std::unique_ptr<Scene> ObjectTemplate::Release () {
    std::unique_ptr<Scene> out = std::move(prefab_);
    Unload();
    return out;
}

Entity ObjectTemplate::Instantiate (Scene& target, const XMFLOAT3& position, const XMFLOAT4& rotation,
                                    wi::vector<Entity>* created) {
    if (!prefab_) return INVALID_ENTITY;

    if (!instanceDataReady_) {
        wi::ecs::EntitySerializer seri;
        instanceData_.SetReadModeAndResetPos(false);
        prefab_->componentLibrary.Serialize(instanceData_, seri);
        instanceData_.SetReadModeAndResetPos(true);
        instanceDataReady_ = true;
    }

    // Read back with remapping on: every entity of the copy gets a fresh id. The prefab's
    // resources are already resident, so the handles resolve without touching the disk.
    Scene tmp;
    wi::ecs::EntitySerializer seri;
    {
        wi::Archive archive(instanceData_.GetData(), instanceData_.GetSize());
        archive.SetReadModeAndResetPos(true);
        tmp.componentLibrary.Serialize(archive, seri);
    }
    wi::jobsystem::Wait(seri.ctx);

    const auto it = seri.remap.find((uint64_t)root_);
    if (it == seri.remap.end()) return INVALID_ENTITY;
    const Entity root = it->second;

    wi::unordered_set<Entity> made;
    tmp.FindAllEntities(made);

    if (TransformComponent* t = tmp.transforms.GetComponent(root)) {
        t->translation_local = position;
        t->rotation_local    = rotation;
        // The 64-bit absolute position too: it is what a save round-trips (see PlaceEntityAt).
        t->SyncWorldFromLocal(wi::scene::GetRenderOrigin());
        t->SetDirty();
        t->UpdateTransform();
    }
    tmp.RunHierarchyUpdateSystem(seri.ctx);
    wi::jobsystem::Wait(seri.ctx);

    target.Merge(tmp);

    // Fresh GUIDs, and the copy's own references re-pointed at them. Minted in the target,
    // after the merge, because that is the scene they have to be unique in.
    std::unordered_map<std::string, std::string> guidMap;
    for (Entity e : made) {
        const MetadataComponent* m = target.metadatas.GetComponent(e);
        if (m == nullptr || !m->string_values.has(kGuidKey)) continue;
        const std::string old = m->string_values.get(kGuidKey);
        guidMap[old] = wi::scene::RegenerateEntityGUID(target, e);
    }
    if (!guidMap.empty()) {
        for (Entity e : made) {
            MetadataComponent* m = target.metadatas.GetComponent(e);
            if (m == nullptr) continue;
            for (size_t v = 0; v < m->string_values.values.size(); ++v) {
                if (!StartsWith(m->string_values.names[v], "NCA_")) continue;
                const auto g = guidMap.find(m->string_values.values[v]);
                if (g != guidMap.end()) m->string_values.values[v] = g->second;
            }
        }
    }

    if (created) {
        created->clear();
        created->push_back(root);
        for (Entity e : made)
            if (e != root) created->push_back(e);
    }
    return root;
}

} // namespace st
