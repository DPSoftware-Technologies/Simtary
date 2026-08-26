#include "imeditorhistory.h"

#include "wiScene.h"
#include "wiBacklog.h"

using wi::ecs::Entity;
using wi::ecs::INVALID_ENTITY;

// ------------------------------------------------------------------ transform ---

st::TransformSnapshot st::TransformSnapshot::Capture(const wi::scene::TransformComponent& t)
{
	TransformSnapshot s;
	s.scale_local       = t.scale_local;
	s.rotation_local    = t.rotation_local;
	s.translation_local = t.translation_local;
	s.worldX = t.GetWorldPositionX();
	s.worldY = t.GetWorldPositionY();
	s.worldZ = t.GetWorldPositionZ();
	s.flags  = t._flags;
	return s;
}

void st::TransformSnapshot::Apply(wi::scene::TransformComponent& t) const
{
	t.scale_local       = scale_local;
	t.rotation_local    = rotation_local;
	t.translation_local = translation_local;
	t.world_translation_x = (wi::scene::world_float)worldX;
	t.world_translation_y = (wi::scene::world_float)worldY;
	t.world_translation_z = (wi::scene::world_float)worldZ;
	t._flags = flags;
	t.SetDirty();
}

bool st::TransformSnapshot::Equals(const TransformSnapshot& o) const
{
	return scale_local.x == o.scale_local.x && scale_local.y == o.scale_local.y && scale_local.z == o.scale_local.z
		&& rotation_local.x == o.rotation_local.x && rotation_local.y == o.rotation_local.y
		&& rotation_local.z == o.rotation_local.z && rotation_local.w == o.rotation_local.w
		&& translation_local.x == o.translation_local.x && translation_local.y == o.translation_local.y
		&& translation_local.z == o.translation_local.z
		&& worldX == o.worldX && worldY == o.worldY && worldZ == o.worldZ;
}

// -------------------------------------------------------------------- helpers ---

namespace {

bool EntityHasAnyComponent(wi::scene::Scene& scene, Entity e)
{
	if (e == INVALID_ENTITY)
		return false;
	for (auto& kv : scene.componentLibrary.entries)
	{
		const auto* mgr = kv.second.component_manager.get();
		if (mgr != nullptr && mgr->Contains(e))
			return true;
	}
	return false;
}

// Strip every component off the entity WITHOUT going through Scene::Entity_Remove, which
//	would also destroy the entity's NativeComponent instances. See the header for why that
//	matters: the metadata we are about to restore names the same components, so the manager
//	would rebuild them from scratch and re-run Awake/Start for what the user sees as an undo.
void ClearEntityComponents(wi::scene::Scene& scene, Entity e)
{
	for (auto& kv : scene.componentLibrary.entries)
	{
		auto* mgr = kv.second.component_manager.get();
		if (mgr != nullptr && mgr->Contains(e))
			mgr->Remove(e);
	}
}

} // namespace

// ------------------------------------------------------------------ recording ---

void st::EditorHistory::BeginTransform(wi::scene::Scene& scene, Entity entity, const char* label)
{
	if (IsRecording() || entity == INVALID_ENTITY)
		return; // a multi-frame drag calls this every frame; only the first one counts
	const wi::scene::TransformComponent* t = scene.transforms.GetComponent(entity);
	if (t == nullptr)
		return;

	pending_ = Step{};
	pending_.kind = Step::Kind::Transform;
	pending_.label = label;
	pending_.entities.push_back(entity);
	pending_.transformsBefore.push_back(TransformSnapshot::Capture(*t));
}

void st::EditorHistory::BeginEntity(wi::scene::Scene& scene, Entity entity, const char* label)
{
	wi::vector<Entity> one;
	one.push_back(entity);
	BeginEntities(scene, one, label);
}

void st::EditorHistory::BeginEntities(wi::scene::Scene& scene,
	const wi::vector<Entity>& entities, const char* label)
{
	if (IsRecording() || entities.empty())
		return;

	pending_ = Step{};
	pending_.kind = Step::Kind::Entity;
	pending_.label = label;
	pending_.entities = entities;
	CaptureInto(scene, pending_, false);
}

void st::EditorHistory::PushTransform(wi::scene::Scene& scene, Entity entity,
	const char* label, const TransformSnapshot& before)
{
	const wi::scene::TransformComponent* t = scene.transforms.GetComponent(entity);
	if (t == nullptr)
		return;

	const TransformSnapshot after = TransformSnapshot::Capture(*t);
	if (after.Equals(before))
		return; // a click on a handle that never moved

	Step step;
	step.kind = Step::Kind::Transform;
	step.label = label;
	step.entities.push_back(entity);
	step.transformsBefore.push_back(before);
	step.transformsAfter.push_back(after);
	Push(std::move(step));
}

void st::EditorHistory::RecordCreated(wi::scene::Scene& scene, Entity entity, const char* label)
{
	if (entity == INVALID_ENTITY)
		return;
	Abort(); // a create is never nested inside another pending capture

	Step step;
	step.kind = Step::Kind::Entity;
	step.label = label;
	step.entities.push_back(entity);
	step.archivesBefore.resize(1);
	step.existedBefore.push_back(0); // "did not exist"
	CaptureInto(scene, step, true);
	Push(std::move(step));
}

void st::EditorHistory::CaptureInto(wi::scene::Scene& scene, Step& step, bool after)
{
	wi::vector<wi::Archive>& archives = after ? step.archivesAfter : step.archivesBefore;
	wi::vector<uint8_t>&     existed  = after ? step.existedAfter  : step.existedBefore;

	archives.clear();
	existed.clear();
	archives.resize(step.entities.size());
	existed.resize(step.entities.size());

	for (size_t i = 0; i < step.entities.size(); ++i)
	{
		const Entity e = step.entities[i];
		const bool exists = EntityHasAnyComponent(scene, e);
		existed[i] = exists ? 1u : 0u;
		if (!exists)
			continue;

		wi::ecs::EntitySerializer seri;
		seri.allow_remap = false; // put the components back on the SAME entity handle

		archives[i].SetReadModeAndResetPos(false);
		scene.Entity_Serialize(archives[i], seri, e,
			wi::scene::Scene::EntitySerializeFlags::KEEP_INTERNAL_ENTITY_REFERENCES);
	}
}

void st::EditorHistory::Commit(wi::scene::Scene& scene)
{
	if (!IsRecording())
		return;

	Step step = std::move(pending_);
	pending_ = Step{};

	if (step.kind == Step::Kind::Transform)
	{
		const wi::scene::TransformComponent* t = scene.transforms.GetComponent(step.entities[0]);
		if (t == nullptr)
			return; // the entity went away mid-edit; nothing sane to record
		step.transformsAfter.push_back(TransformSnapshot::Capture(*t));
		if (step.transformsAfter[0].Equals(step.transformsBefore[0]))
			return; // a click that did not actually move anything
	}
	else
	{
		CaptureInto(scene, step, true);
	}

	Push(std::move(step));
}

void st::EditorHistory::Abort()
{
	pending_ = Step{};
}

void st::EditorHistory::Push(Step&& step)
{
	// A new edit invalidates the redo branch, exactly like every other editor.
	redo_.clear();
	undo_.push_back(std::move(step));
	if (undo_.size() > MAX_STEPS)
		undo_.erase(undo_.begin());
}

// ------------------------------------------------------------------- applying ---

void st::EditorHistory::ApplyState(wi::scene::Scene& scene, Step& step, bool useAfter)
{
	if (step.kind == Step::Kind::Transform)
	{
		const TransformSnapshot& snap = useAfter ? step.transformsAfter[0] : step.transformsBefore[0];
		if (wi::scene::TransformComponent* t = scene.transforms.GetComponent(step.entities[0]))
			snap.Apply(*t);
		return;
	}

	wi::vector<wi::Archive>& archives = useAfter ? step.archivesAfter : step.archivesBefore;
	wi::vector<uint8_t>&     existed  = useAfter ? step.existedAfter  : step.existedBefore;

	for (size_t i = 0; i < step.entities.size(); ++i)
	{
		const Entity e = step.entities[i];

		// Wipe what is there now, then lay the snapshot back down. Doing it in this order is
		// what makes "undo an Add Component" work: the archive simply does not contain it.
		ClearEntityComponents(scene, e);

		if (i >= existed.size() || existed[i] == 0)
			continue; // the entity did not exist in this state — leaving it bare removes it

		wi::ecs::EntitySerializer seri;
		seri.allow_remap = false;
		archives[i].SetReadModeAndResetPos(true);
		scene.Entity_Serialize(archives[i], seri, INVALID_ENTITY,
			wi::scene::Scene::EntitySerializeFlags::KEEP_INTERNAL_ENTITY_REFERENCES);
	}
}

Entity st::EditorHistory::Undo(wi::scene::Scene& scene)
{
	if (undo_.empty())
		return INVALID_ENTITY;

	Step step = std::move(undo_.back());
	undo_.pop_back();
	ApplyState(scene, step, false);
	wi::backlog::post("Editor: undo " + step.label);

	const Entity selected = step.entities.empty() ? INVALID_ENTITY : step.entities[0];
	redo_.push_back(std::move(step));
	return selected;
}

Entity st::EditorHistory::Redo(wi::scene::Scene& scene)
{
	if (redo_.empty())
		return INVALID_ENTITY;

	Step step = std::move(redo_.back());
	redo_.pop_back();
	ApplyState(scene, step, true);
	wi::backlog::post("Editor: redo " + step.label);

	const Entity selected = step.entities.empty() ? INVALID_ENTITY : step.entities[0];
	undo_.push_back(std::move(step));
	return selected;
}

std::string st::EditorHistory::UndoLabel() const
{
	return undo_.empty() ? std::string() : undo_.back().label;
}

std::string st::EditorHistory::RedoLabel() const
{
	return redo_.empty() ? std::string() : redo_.back().label;
}

void st::EditorHistory::Clear()
{
	pending_ = Step{};
	undo_.clear();
	redo_.clear();
}
