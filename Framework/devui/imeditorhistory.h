#pragma once
// Undo / redo for Editor mode (Ctrl+Z / Ctrl+Y).
//
//	Two step kinds, because one size does not fit here:
//
//	  Transform  - the common case: a gizmo drag, or a drag on one of the Properties
//	               transform fields. Stores nothing but the TransformComponent's own
//	               values, so undoing is a plain write plus SetDirty(). No component is
//	               destroyed and rebuilt, so nothing else on the entity notices.
//
//	  Entity     - structural edits: add/remove a component, attach/detach a native
//	               component, create or delete an entity. Stores a wi::Archive snapshot of
//	               each affected entity's full component set, taken with
//	               EntitySerializer::allow_remap = false so the entity handles come back
//	               exactly as they were and every other entity's references stay valid.
//
//	An Entity restore clears the entity's components through the ComponentLibrary rather
//	than through Scene::Entity_Remove: Entity_Remove would also destroy the entity's
//	NativeComponent instances, and since the restored MetadataComponent carries the same
//	NCI_ keys the manager would just build them again a frame later - Awake/Start and all.
//	Clearing at the component-manager level leaves the live instances alone, so undoing a
//	component edit does not silently restart a game object's script state.
//
//	Usage is a Begin/Commit bracket around the edit:
//
//	    history.BeginTransform(scene, entity, "Move");
//	    ... the gizmo drags ...
//	    history.Commit(scene);
//
//	Begin captures "before", Commit captures "after" and pushes the step. Abort() throws
//	the pending capture away (a drag that ended up changing nothing). Beginning a second
//	step while one is pending is a no-op, so a drag that spans many frames can call
//	BeginTransform every frame without stacking up.

#include "wiArchive.h"
#include "wiECS.h"
#include "wiVector.h"

#include <string>

namespace wi::scene { struct Scene; struct TransformComponent; }

namespace st {

// Everything a TransformComponent needs to be put back exactly as it was, including the
//	64-bit large-world absolute position and the LARGE_WORLD/DIRTY flags.
struct TransformSnapshot {
    XMFLOAT3 scale_local       = XMFLOAT3(1, 1, 1);
    XMFLOAT4 rotation_local    = XMFLOAT4(0, 0, 0, 1);
    XMFLOAT3 translation_local = XMFLOAT3(0, 0, 0);
    double   worldX = 0, worldY = 0, worldZ = 0;
    uint32_t flags = 0;

    static TransformSnapshot Capture(const wi::scene::TransformComponent& t);
    void Apply(wi::scene::TransformComponent& t) const;
    bool Equals(const TransformSnapshot& other) const;
};

class EditorHistory {
public:
    // Maximum steps kept. Older ones drop off the bottom.
    static constexpr size_t MAX_STEPS = 128;

    // recording
    // Cheap value-only capture of one entity's transform.
    void BeginTransform(wi::scene::Scene& scene, wi::ecs::Entity entity, const char* label);
    // Archive capture of one entity's whole component set.
    void BeginEntity(wi::scene::Scene& scene, wi::ecs::Entity entity, const char* label);
    // Same, for several entities at once (an entity and its subtree, say).
    void BeginEntities(wi::scene::Scene& scene, const wi::vector<wi::ecs::Entity>& entities,
        const char* label);
    // Push a transform step whose "before" the caller already holds. The gizmo captures the
    //	transform on every frame it is NOT dragging, so by the time ImGuizmo reports a drag the
    //	pre-drag value is already in hand - earlier than any Begin/Commit bracket could open.
    void PushTransform(wi::scene::Scene& scene, wi::ecs::Entity entity, const char* label,
        const TransformSnapshot& before);

    // Record an entity that has JUST been created. There is no earlier state to capture, so
    //	"before" is synthesised as "this entity had no components" - undoing the step strips
    //	it back to that, which is what removing it means to every panel here.
    void RecordCreated(wi::scene::Scene& scene, wi::ecs::Entity entity, const char* label);
    // Same, for a batch that appeared together. A model import is the case this exists for:
    //	it creates one root plus every mesh, material, armature and animation entity behind
    //	it, and only a step that owns ALL of them can undo the import rather than orphan the
    //	contents of a deleted root. The caller works out the set by diffing the scene's entity
    //	list around the load.
    void RecordCreatedMany(wi::scene::Scene& scene, const wi::vector<wi::ecs::Entity>& entities,
        const char* label);

    // Capture the "after" state and push the step. Drops the step if nothing changed.
    void Commit(wi::scene::Scene& scene);
    // Throw the pending capture away.
    void Abort();
    bool IsRecording() const { return pending_.kind != Step::Kind::None; }

    // applying
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }
    // Label of the step Ctrl+Z / Ctrl+Y would apply, for the menu. "" when there is none.
    std::string UndoLabel() const;
    std::string RedoLabel() const;
    // Both return the entity worth selecting afterwards, or INVALID_ENTITY.
    wi::ecs::Entity Undo(wi::scene::Scene& scene);
    wi::ecs::Entity Redo(wi::scene::Scene& scene);

    void Clear();

private:
    struct Step {
        enum class Kind { None, Transform, Entity } kind = Kind::None;
        std::string label;
        wi::vector<wi::ecs::Entity> entities;

        // Kind::Transform
        wi::vector<TransformSnapshot> transformsBefore;
        wi::vector<TransformSnapshot> transformsAfter;

        // Kind::Entity - one archive per entity, in `entities` order. `existed` records
        // whether the entity had any components at all, which is how create/delete undo works.
        wi::vector<wi::Archive> archivesBefore;
        wi::vector<wi::Archive> archivesAfter;
        wi::vector<uint8_t>     existedBefore;
        wi::vector<uint8_t>     existedAfter;
    };

    void CaptureInto(wi::scene::Scene& scene, Step& step, bool after);
    void ApplyState(wi::scene::Scene& scene, Step& step, bool useAfter);
    void Push(Step&& step);

    Step pending_;
    wi::vector<Step> undo_;
    wi::vector<Step> redo_;
};

} // namespace st
