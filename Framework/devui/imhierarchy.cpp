#include "imhierarchy.h"
#include "imcomponents.h"
#include "imeditorhistory.h"

#include "stNativeComponent.h"
#include "wiScene.h"
#include "wiMath.h"
#include "imgui.h"
#include "imgui_internal.h"   // IsMouseDragPastThreshold(): click-vs-drag test on the release frame
#include "ImGuizmo.h"   // IsUsingAny(): tells the drift check a drag is in progress

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cstdio>

using wi::ecs::Entity;
using wi::ecs::INVALID_ENTITY;
using namespace wi::scene;

// ------------------------------------------------------------------ helpers ---

static std::string EntityLabel(Scene& scene, Entity e)
{
	if (const NameComponent* n = scene.names.GetComponent(e); n && !n->name.empty())
		return n->name;
	return "Entity " + std::to_string((unsigned)e);
}

// Strip the "wi::scene::Scene::" namespace prefix off a ComponentLibrary key so the
//	inspector shows "transforms" rather than the fully-qualified registration name.
static std::string ShortCompName(const std::string& full)
{
	static const std::string prefix = "wi::scene::Scene::";
	if (full.size() > prefix.size() && full.compare(0, prefix.size(), prefix) == 0)
		return full.substr(prefix.size());
	return full;
}

// What the Hierarchy is doing right now: INVALID_ENTITY when idle, otherwise the entity the
//	user is dragging out of a row. Read straight off ImGui's live payload, so it is correct on
//	the same frame the drag starts and can never get stuck "dragging" if the drag ends outside
//	the window.
static Entity CurrentDragEntity()
{
	const ImGuiPayload* p = ImGui::GetDragDropPayload();
	if (p == nullptr || !p->IsDataType(SIMTARY_ENTITY_PAYLOAD)) return INVALID_ENTITY;
	if (p->DataSize != (int)sizeof(Entity))                     return INVALID_ENTITY;
	return *(const Entity*)p->Data;
}

// Shared row behaviour for both hierarchy views: publish the drag payload, and select ONLY on a
//	real click.
//
//	A click here means press and release on the same row without dragging past ImGui's drag
//	threshold. Selecting on press (IsItemClicked) meant that starting a drag also re-selected the
//	row, which swapped the Properties panel -- and with it the EntityField you were dragging onto
//	-- out from under the cursor mid-drag. Deferring to release separates "I want this entity" from
//	"I want to carry this entity somewhere".
static void HierarchyRowInteract(Scene& scene, Entity e, Entity& selected)
{
	// Snapshot before BeginDragDropSource, which overwrites the last-item state.
	const bool hovered      = ImGui::IsItemHovered();
	const bool toggledOpen  = ImGui::IsItemToggledOpen();   // click landed on the expand arrow

	// Drag source: lets you drag this entity onto an EntityField (Unity-style object reference).
	if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
	{
		ImGui::SetDragDropPayload(SIMTARY_ENTITY_PAYLOAD, &e, sizeof(Entity));
		ImGui::Text("%s", EntityLabel(scene, e).c_str());
		ImGui::EndDragDropSource();
	}

	if (hovered && !toggledOpen
		&& ImGui::IsMouseReleased(ImGuiMouseButton_Left)
		&& !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left))
		selected = e;
}

static float Deg(float rad) { return rad * (180.0f / XM_PI); }
static float Rad(float deg) { return deg * (XM_PI / 180.0f); }

// Union of every entity that owns at least one component of any registered type.
static void GatherEntities(Scene& scene, std::unordered_set<Entity>& out)
{
	for (auto& kv : scene.componentLibrary.entries)
	{
		const auto* mgr = kv.second.component_manager.get();
		if (!mgr) continue;
		for (Entity e : mgr->GetEntityArray())
			out.insert(e);
	}
	// Native components can live on an entity that has no engine component yet.
	for (auto& kv : scene.nativeComponents.instances)
		out.insert(kv.first);
}

static bool EntityExists(Scene& scene, Entity e)
{
	if (e == INVALID_ENTITY) return false;
	for (auto& kv : scene.componentLibrary.entries)
	{
		const auto* mgr = kv.second.component_manager.get();
		if (mgr && mgr->Contains(e)) return true;
	}
	return scene.nativeComponents.instances.count(e) > 0;
}

// ---------------------------------------------------------------- hierarchy ---

// Collect an entity and every descendant, deepest first, so a delete step can snapshot the
//	whole subtree before Scene::Entity_Remove takes it apart.
static void GatherSubtree(Scene& scene, Entity root, wi::vector<Entity>& out)
{
	out.push_back(root);
	for (size_t i = 0; i < scene.hierarchy.GetCount(); ++i)
	{
		if (scene.hierarchy[i].parentID == root)
			GatherSubtree(scene, scene.hierarchy.GetEntity(i), out);
	}
}

// Right-click menu on a Hierarchy row (and on empty space, with e == INVALID_ENTITY).
//	Only drawn when the caller supplied a history, i.e. in Editor mode.
static void EntityContextMenu(Scene& scene, Entity e, Entity& selected, st::EditorHistory& history)
{
	if (!ImGui::BeginPopupContextItem())
		return;

	// Spawn at the origin: this menu has no camera to aim from, and the editor's own
	// Create menu (which does) is the one that places things in front of you.
	if (ImGui::BeginMenu("Create child"))
	{
		const Entity created = CreateObjectMenuItems(scene, XMFLOAT3(0, 0, 0), e, &history);
		if (created != INVALID_ENTITY)
			selected = created;
		ImGui::EndMenu();
	}

	ImGui::Separator();

	if (ImGui::MenuItem("Duplicate"))
	{
		const Entity clone = scene.Entity_Duplicate(e);
		if (clone != INVALID_ENTITY)
		{
			// Entity_Duplicate copies the stable GUID too, which would make two entities
			// answer to the same reference. Re-stamp the clone (see stNativeComponent.h).
			RegenerateEntityGUID(scene, clone);
			history.RecordCreated(scene, clone, "Duplicate");
			selected = clone;
		}
	}

	if (ImGui::MenuItem("Delete"))
	{
		wi::vector<Entity> subtree;
		GatherSubtree(scene, e, subtree);
		history.BeginEntities(scene, subtree, "Delete");
		scene.Entity_Remove(e, true);
		history.Commit(scene);
		if (selected == e)
			selected = INVALID_ENTITY;
	}

	ImGui::EndPopup();
}

static void DrawNode(Scene& scene, Entity e,
	const std::unordered_map<Entity, std::vector<Entity>>& children,
	Entity& selected, std::unordered_set<Entity>& visited, st::EditorHistory* history)
{
	if (!visited.insert(e).second) return; // cycle guard: never recurse an entity twice

	auto it = children.find(e);
	const bool hasChildren = (it != children.end() && !it->second.empty());

	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (e == selected)   flags |= ImGuiTreeNodeFlags_Selected;
	if (!hasChildren)    flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

	// "##<id>" keeps the visible label clean while giving every row a unique ImGui id.
	const std::string label = EntityLabel(scene, e) + "##" + std::to_string((unsigned)e);
	// Tint the row that is currently being carried, so the tree shows what the drag is holding.
	const bool isDragged = (e == CurrentDragEntity());
	if (isDragged)
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.78f, 0.25f, 1.00f));

	const bool open = ImGui::TreeNodeEx(label.c_str(), flags);

	if (isDragged)
		ImGui::PopStyleColor();

	HierarchyRowInteract(scene, e, selected);

	if (history != nullptr)
		EntityContextMenu(scene, e, selected, *history);

	if (open && hasChildren)
	{
		for (Entity c : it->second)
			DrawNode(scene, c, children, selected, visited, history);
		ImGui::TreePop();
	}
}

void HierarchyGUI(Scene& scene, Entity& selected, st::EditorHistory* history)
{
	// Editor mode gets the create/delete toolbar; the plain DevUI window stays a viewer.
	if (history != nullptr)
	{
		if (ImGui::Button("+ Create"))
			ImGui::OpenPopup("##hierarchy_create");
		if (ImGui::BeginPopup("##hierarchy_create"))
		{
			const Entity created = CreateObjectMenuItems(scene, XMFLOAT3(0, 0, 0), INVALID_ENTITY, history);
			if (created != INVALID_ENTITY)
				selected = created;
			ImGui::EndPopup();
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(selected == INVALID_ENTITY);
		if (ImGui::Button("Delete"))
		{
			wi::vector<Entity> subtree;
			GatherSubtree(scene, selected, subtree);
			history->BeginEntities(scene, subtree, "Delete");
			scene.Entity_Remove(selected, true);
			history->Commit(scene);
			selected = INVALID_ENTITY;
		}
		ImGui::EndDisabled();
	}

	static ImGuiTextFilter filter;
	filter.Draw("Filter", -1.0f);

	std::unordered_set<Entity> all;
	GatherEntities(scene, all);

	ImGui::Text("%zu entities", all.size());
	ImGui::SameLine();
	if (ImGui::SmallButton("Deselect"))
		selected = INVALID_ENTITY;

	// Mode readout: tells you whether the next mouse-up selects a row or drops a reference.
	if (const Entity dragging = CurrentDragEntity(); dragging != INVALID_ENTITY)
		ImGui::TextColored(ImVec4(1.00f, 0.78f, 0.25f, 1.00f),
			"Dragging \"%s\" -- drop it on an entity field (selection kept)",
			EntityLabel(scene, dragging).c_str());
	else
		ImGui::TextDisabled("Click a row to select -- drag a row onto an entity field to reference it");

	ImGui::Separator();

	ImGui::BeginChild("##hierarchy_tree", ImVec2(0, 0), false);

	if (filter.IsActive())
	{
		// Flat, filtered view: a tree is meaningless once you're searching by name.
		std::vector<Entity> matches;
		matches.reserve(all.size());
		for (Entity e : all)
		{
			const std::string name = EntityLabel(scene, e);
			if (filter.PassFilter(name.c_str()))
				matches.push_back(e);
		}
		std::sort(matches.begin(), matches.end(), [&](Entity a, Entity b) {
			return EntityLabel(scene, a) < EntityLabel(scene, b);
		});
		for (Entity e : matches)
		{
			const std::string label = EntityLabel(scene, e) + "##" + std::to_string((unsigned)e);

			const bool isDragged = (e == CurrentDragEntity());
			if (isDragged)
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.78f, 0.25f, 1.00f));

			// Selection is applied by HierarchyRowInteract on release, not by Selectable's own
			//	press-time return, so a drag out of the list leaves the selection alone.
			ImGui::Selectable(label.c_str(), e == selected);

			if (isDragged)
				ImGui::PopStyleColor();

			HierarchyRowInteract(scene, e, selected);
			if (history != nullptr)
				EntityContextMenu(scene, e, selected, *history);
		}
	}
	else
	{
		// Build the parent -> children map from HierarchyComponent links. Entities with no
		//	(in-scene) parent become roots.
		std::unordered_map<Entity, std::vector<Entity>> children;
		std::vector<Entity> roots;
		for (Entity e : all)
		{
			Entity parent = INVALID_ENTITY;
			if (const HierarchyComponent* h = scene.hierarchy.GetComponent(e))
				parent = h->parentID;
			if (parent != INVALID_ENTITY && all.count(parent))
				children[parent].push_back(e);
			else
				roots.push_back(e);
		}

		auto byName = [&](Entity a, Entity b) { return EntityLabel(scene, a) < EntityLabel(scene, b); };
		std::sort(roots.begin(), roots.end(), byName);
		for (auto& kv : children)
			std::sort(kv.second.begin(), kv.second.end(), byName);

		std::unordered_set<Entity> visited;
		for (Entity e : roots)
			DrawNode(scene, e, children, selected, visited, history);
	}

	ImGui::EndChild();
}

void HierarchyWindow(Scene& scene, Entity& selected, bool* p_open)
{
	ImGui::SetNextWindowSize(ImVec2(280, 420), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Hierarchy", p_open))
		HierarchyGUI(scene, selected);
	ImGui::End();
}

// --------------------------------------------------------------- properties ---

// Inline std::string text edit through a fixed scratch buffer.
static bool EditString(const char* label, std::string& s)
{
	char buf[256];
	std::snprintf(buf, sizeof(buf), "%s", s.c_str());
	if (ImGui::InputText(label, buf, sizeof(buf)))
	{
		s = buf;
		return true;
	}
	return false;
}

// One component's collapsing header plus the right-aligned "x" that detaches it.
//	Returns true when the caller should draw the component's body — i.e. the header is open
//	AND the component still exists. The "x" calls ComponentManager::Remove immediately, so
//	any pointer the caller is holding is dangling the moment this returns false.
static bool ComponentHeader(Scene& scene, Entity e, const char* libraryKey,
	const char* label, bool defaultOpen, st::EditorHistory* history)
{
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_AllowOverlap;
	if (defaultOpen) flags |= ImGuiTreeNodeFlags_DefaultOpen;

	const bool open = ImGui::CollapsingHeader(label, flags);

	bool removed = false;
	if (const EngineComponentType* type = FindEngineComponentByKey(libraryKey))
		removed = RemoveEngineComponentButton(scene, e, *type, history);

	return open && !removed;
}

static void DrawTransform(Scene& scene, Entity e, st::EditorHistory* history)
{
	TransformComponent* t = scene.transforms.GetComponent(e);
	if (!t) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::transforms", "Transform", true, history)) return;

	// Euler is cached rather than re-extracted every frame, so dragging the rotation field
	//	doesn't fight the quaternion<->euler round-trip (re-extracting each frame jitters the
	//	values). The cache also remembers WHICH quaternion it was derived from: when something
	//	else rewrites the rotation - the gizmo, an undo, an animation - the two no longer match
	//	and the euler is rebuilt. Without that the field sat at 0,0,0 after a gizmo rotate.
	static Entity   eulerEntity = INVALID_ENTITY;
	static XMFLOAT3 eulerDeg    = XMFLOAT3(0, 0, 0);
	static XMFLOAT4 eulerSource = XMFLOAT4(0, 0, 0, 1);
	const bool rotatedElsewhere =
		t->rotation_local.x != eulerSource.x || t->rotation_local.y != eulerSource.y ||
		t->rotation_local.z != eulerSource.z || t->rotation_local.w != eulerSource.w;
	if (eulerEntity != e || rotatedElsewhere)
	{
		const XMFLOAT3 rpy = wi::math::QuaternionToRollPitchYaw(t->rotation_local);
		eulerDeg = XMFLOAT3(Deg(rpy.x), Deg(rpy.y), Deg(rpy.z));
		eulerSource = t->rotation_local;
		eulerEntity = e;
	}

	// One undo step per drag, not one per frame: open the step when the widget takes the
	//	mouse and close it when the widget gives it back having actually changed something.
	auto trackEdit = [&](const char* label) {
		if (history == nullptr) return;
		if (ImGui::IsItemActivated())            history->BeginTransform(scene, e, label);
		if (ImGui::IsItemDeactivatedAfterEdit()) history->Commit(scene);
		else if (ImGui::IsItemDeactivated())     history->Abort();
	};

	// ── is something else driving this transform? ────────────────────────────────
	//	An animation, a physics body or a native component that writes translation_local
	//	every frame owns this transform, and a gizmo drag into the same field is overwritten
	//	before the next frame is drawn — which looks exactly like the object being locked.
	//	Watch the value while nobody is editing it: if it moves on its own, say so.
	static Entity   driftEntity = INVALID_ENTITY;
	static XMFLOAT3 driftLast   = XMFLOAT3(0, 0, 0);
	static int      driftFrames = 0;
	{
		const bool beingEdited = ImGui::IsAnyItemActive() || ImGuizmo::IsUsingAny();
		if (driftEntity != e)
		{
			driftEntity = e;
			driftFrames = 0;
		}
		else if (!beingEdited)
		{
			const bool moved =
				driftLast.x != t->translation_local.x ||
				driftLast.y != t->translation_local.y ||
				driftLast.z != t->translation_local.z;
			// A couple of consecutive frames, so one-off writes (an undo, a scene load)
			// do not raise it.
			driftFrames = moved ? driftFrames + 1 : 0;
		}
		driftLast = t->translation_local;
	}

	if (driftFrames >= 3)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
		ImGui::TextWrapped(
			"Position (local) is being rewritten every frame by something else - an "
			"animation, a physics body, or one of the native components below. Dragging the "
			"gizmo or this field will not stick. Edit World pos (abs) instead (it wins on a "
			"large-world transform), or untick the driver's 'enabled' box first.");
		ImGui::PopStyleColor();
	}

	// Editing the local offset has to push through to the ABSOLUTE position, for parented
	//	transforms as much as for roots. On a LARGE_WORLD transform translation_local is
	//	derived - UpdateTransform rebases it from world_translation_* every frame, children
	//	included - so a write that stopped at the local field would be gone next frame. And
	//	the absolute position is the one that gets serialized either way.
	bool dirty = false;
	if (ImGui::DragFloat3("Position (local)", &t->translation_local.x, 0.01f))
	{
		t->SyncWorldFromLocal(wi::scene::GetRenderOrigin());
		dirty = true;
	}
	trackEdit("Move");

	if (ImGui::DragFloat3("Rotation (deg)", &eulerDeg.x, 0.5f))
	{
		const XMVECTOR q = XMQuaternionRotationRollPitchYaw(Rad(eulerDeg.x), Rad(eulerDeg.y), Rad(eulerDeg.z));
		XMStoreFloat4(&t->rotation_local, q);
		eulerSource = t->rotation_local; // this change is ours: don't re-extract over the drag
		dirty = true;
	}
	trackEdit("Rotate");

	dirty |= ImGui::DragFloat3("Scale", &t->scale_local.x, 0.01f);
	trackEdit("Scale");

	// Simtary 64-bit large-world absolute position.
	double wp[3] = { t->world_translation_x, t->world_translation_y, t->world_translation_z };
	if (ImGui::DragScalarN("World pos (abs)", ImGuiDataType_Double, wp, 3, 0.1f)) {
		t->SetWorldPosition(wp[0], wp[1], wp[2]);
	}
	trackEdit("Set World Position");

	if (dirty)
		t->SetDirty();
}

static void DrawLight(Scene& scene, Entity e, st::EditorHistory* history)
{
	LightComponent* l = scene.lights.GetComponent(e);
	if (!l) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::lights", "Light", true, history)) return;

	const char* types[] = { "Directional", "Point", "Spot", "Rectangle" };
	int ty = (int)l->type;
	if (ImGui::Combo("Type", &ty, types, IM_ARRAYSIZE(types)))
		l->type = (LightComponent::LightType)ty;

	ImGui::ColorEdit3("Color", &l->color.x);
	ImGui::DragFloat("Intensity", &l->intensity, 0.5f, 0.0f, 100000.0f);
	ImGui::DragFloat("Range", &l->range, 0.1f, 0.0f, 100000.0f);
	ImGui::DragFloat("Radius", &l->radius, 0.001f, 0.0f, 1000.0f);

	if (l->type == LightComponent::SPOT)
	{
		ImGui::DragFloat("Outer cone", &l->outerConeAngle, 0.01f, 0.0f, XM_PIDIV2);
		ImGui::DragFloat("Inner cone", &l->innerConeAngle, 0.01f, 0.0f, XM_PIDIV2);
	}

	bool cast = l->IsCastingShadow();
	if (ImGui::Checkbox("Cast shadow", &cast)) l->SetCastShadow(cast);
	bool vol = l->IsVolumetricsEnabled();
	if (ImGui::Checkbox("Volumetrics", &vol)) l->SetVolumetricsEnabled(vol);
}

static void DrawCamera(Scene& scene, Entity e, st::EditorHistory* history)
{
	CameraComponent* c = scene.cameras.GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::cameras", "Camera", false, history)) return;

	ImGui::DragFloat("FOV (rad)", &c->fov, 0.01f, 0.01f, XM_PI - 0.01f);
	ImGui::DragFloat("Near", &c->zNearP, 0.01f, 0.001f, 100000.0f);
	ImGui::DragFloat("Far", &c->zFarP, 1.0f, 0.01f, 1000000.0f);
	ImGui::DragFloat("Focal length", &c->focal_length, 0.01f);
	ImGui::DragFloat("Aperture size", &c->aperture_size, 0.01f, 0.0f, 100.0f);
}

static void DrawObject(Scene& scene, Entity e, st::EditorHistory* history)
{
	ObjectComponent* o = scene.objects.GetComponent(e);
	if (!o) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::objects", "Object", true, history)) return;

	ImGui::ColorEdit4("Color", &o->color.x);
	ImGui::ColorEdit4("Emissive", &o->emissiveColor.x);
	ImGui::DragFloat("LOD bias", &o->lod_bias, 0.01f);
	ImGui::DragFloat("Draw distance", &o->draw_distance, 1.0f, 0.0f, 1000000.0f);

	bool renderable = (o->_flags & ObjectComponent::RENDERABLE) != 0;
	if (ImGui::Checkbox("Renderable", &renderable)) o->SetRenderable(renderable);
	bool cast = o->IsCastingShadow();
	if (ImGui::Checkbox("Cast shadow", &cast)) o->SetCastShadow(cast);

	if (o->meshID != INVALID_ENTITY)
		ImGui::TextDisabled("mesh: %s", EntityLabel(scene, o->meshID).c_str());
}

static void DrawMaterial(Scene& scene, Entity e, st::EditorHistory* history)
{
	MaterialComponent* m = scene.materials.GetComponent(e);
	if (!m) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::materials", "Material", true, history)) return;

	bool dirty = false;
	dirty |= ImGui::ColorEdit4("Base color", &m->baseColor.x);
	dirty |= ImGui::ColorEdit3("Emissive", &m->emissiveColor.x);
	dirty |= ImGui::DragFloat("Emissive strength", &m->emissiveColor.w, 0.01f, 0.0f, 1000.0f);
	dirty |= ImGui::DragFloat("Roughness", &m->roughness, 0.005f, 0.0f, 1.0f);
	dirty |= ImGui::DragFloat("Metalness", &m->metalness, 0.005f, 0.0f, 1.0f);
	dirty |= ImGui::DragFloat("Reflectance", &m->reflectance, 0.005f, 0.0f, 1.0f);

	bool doubleSided = m->IsDoubleSided();
	if (ImGui::Checkbox("Double sided", &doubleSided)) { m->SetDoubleSided(doubleSided); dirty = true; }

	if (dirty)
		m->SetDirty();
}

static void DrawLayer(Scene& scene, Entity e, st::EditorHistory* history)
{
	LayerComponent* layer = scene.layers.GetComponent(e);
	if (!layer) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::layers", "Layer", false, history)) return;

	int mask = (int)layer->layerMask;
	if (ImGui::InputScalar("Layer mask", ImGuiDataType_S32, &mask, nullptr, nullptr, "%08X", ImGuiInputTextFlags_CharsHexadecimal))
		layer->layerMask = (uint32_t)mask;
}

static void DrawName(Scene& scene, Entity e, st::EditorHistory* history)
{
	NameComponent* n = scene.names.GetComponent(e);
	if (!n) return;

	// Leave room on the row for the detach button RemoveEngineComponentButton parks at the
	//	right edge, otherwise the field draws underneath it.
	const float reserved = ImGui::GetFrameHeight() + ImGui::CalcTextSize("Name").x +
		ImGui::GetStyle().ItemSpacing.x * 2.0f;
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - reserved);
	EditString("Name", n->name);

	if (const EngineComponentType* type = FindEngineComponentByKey("wi::scene::Scene::names"))
		RemoveEngineComponentButton(scene, e, *type, history);
}

static void DrawMetadata(Scene& scene, Entity e, st::EditorHistory* history)
{
	MetadataComponent* md = scene.metadatas.GetComponent(e);
	if (!md) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::metadatas", "Metadata", false, history)) return;

	ImGui::TextDisabled("Native-component attachments (NCI_/NCA_/NCE_) live here.");

	ImGui::PushID("md_bool");
	for (size_t i = 0; i < md->bool_values.names.size(); ++i)
	{
		ImGui::PushID((int)i);
		bool v = md->bool_values.values[i];
		if (ImGui::Checkbox(md->bool_values.names[i].c_str(), &v))
			md->bool_values.values[i] = v;
		ImGui::PopID();
	}
	ImGui::PopID();

	ImGui::PushID("md_int");
	for (size_t i = 0; i < md->int_values.names.size(); ++i)
	{
		ImGui::PushID((int)i);
		int v = md->int_values.values[i];
		if (ImGui::InputInt(md->int_values.names[i].c_str(), &v))
			md->int_values.values[i] = v;
		ImGui::PopID();
	}
	ImGui::PopID();

	ImGui::PushID("md_float");
	for (size_t i = 0; i < md->float_values.names.size(); ++i)
	{
		ImGui::PushID((int)i);
		float v = md->float_values.values[i];
		if (ImGui::DragFloat(md->float_values.names[i].c_str(), &v, 0.01f))
			md->float_values.values[i] = v;
		ImGui::PopID();
	}
	ImGui::PopID();

	ImGui::PushID("md_string");
	for (size_t i = 0; i < md->string_values.names.size(); ++i)
	{
		ImGui::PushID((int)i);
		EditString(md->string_values.names[i].c_str(), md->string_values.values[i]);
		ImGui::PopID();
	}
	ImGui::PopID();
}

static void DrawNativeComponents(Scene& scene, Entity e, st::EditorHistory* history)
{
	auto it = scene.nativeComponents.instances.find(e);
	if (it == scene.nativeComponents.instances.end() || it->second.empty())
		return;
	if (!ImGui::CollapsingHeader("Native Components", ImGuiTreeNodeFlags_DefaultOpen)) return;

	for (NativeComponentManager::Instance& inst : it->second)
	{
		if (!inst.component) continue;
		ImGui::PushID(inst.localID);
		const std::string title = inst.name + " [" + std::to_string(inst.localID) + "]";
		const bool open = ImGui::TreeNodeEx(title.c_str(),
			ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

		// Detach only erases the NCI_/NCA_/NCE_ metadata keys. The instance's OnDisable()
		//	and Destroy() run in the next NativeComponentManager::RunUpdate, so `inst` stays
		//	valid for the rest of this frame — but its widgets would be editing something
		//	already on its way out, so stop drawing it here.
		ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
		const bool detached = ImGui::SmallButton("x");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Detach %s (NCI_%d) from this entity", inst.name.c_str(), inst.localID);
		if (detached)
		{
			if (history) history->BeginEntity(scene, e, "Detach Component");
			DetachNativeComponent(scene, e, inst.localID);
			if (history) history->Commit(scene);
		}

		if (open)
		{
			if (!detached)
			{
				bool enabled = inst.component->IsEnabled();
				if (ImGui::Checkbox("enabled", &enabled))
					inst.component->SetEnabled(enabled); // writes NCE_<id> metadata (persisted)
				ImGui::SameLine();
				ImGui::TextDisabled(inst.started ? "(started)" : "(awaiting start)");

				ImGui::BeginDisabled(!enabled);
				inst.component->DrawDebug(); // component-supplied widgets bound to live members
				ImGui::EndDisabled();
			}
			else
			{
				ImGui::TextDisabled("detaching...");
			}
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
}

void PropertiesGUI(Scene& scene, Entity& selected, st::EditorHistory* history)
{
	if (selected == INVALID_ENTITY)
	{
		ImGui::TextDisabled("No entity selected.");
		ImGui::TextDisabled("Pick one in the Hierarchy window.");
		return;
	}
	if (!EntityExists(scene, selected))
	{
		// Scene reloaded / entity removed out from under us -> drop the stale selection.
		ImGui::TextDisabled("Selected entity no longer exists.");
		selected = INVALID_ENTITY;
		return;
	}

	ImGui::Text("Entity %u", (unsigned)selected);
	ImGui::SameLine();
	if (ImGui::SmallButton("Deselect")) { selected = INVALID_ENTITY; return; }
	ImGui::Separator();

	// Rich, realtime editors for the common engine components.
	DrawName(scene, selected, history);
	DrawTransform(scene, selected, history);
	DrawObject(scene, selected, history);
	DrawMaterial(scene, selected, history);
	DrawLight(scene, selected, history);
	DrawCamera(scene, selected, history);
	DrawLayer(scene, selected, history);
	DrawMetadata(scene, selected, history);

	// Native components.
	DrawNativeComponents(scene, selected, history);

	// Completeness: list every OTHER engine component present that has no inline editor,
	//	so the inspector always reflects the full component set on the entity.
	static const std::unordered_set<std::string> handled = {
		"wi::scene::Scene::names",
		"wi::scene::Scene::transforms",
		"wi::scene::Scene::objects",
		"wi::scene::Scene::materials",
		"wi::scene::Scene::lights",
		"wi::scene::Scene::cameras",
		"wi::scene::Scene::layers",
		"wi::scene::Scene::metadatas",
	};

	std::vector<std::string> others;
	for (auto& kv : scene.componentLibrary.entries)
	{
		const auto* mgr = kv.second.component_manager.get();
		if (!mgr || !mgr->Contains(selected)) continue;
		if (handled.count(kv.first)) continue;
		others.push_back(kv.first); // full library key: the detach table is keyed by it
	}
	std::sort(others.begin(), others.end());

	if (!others.empty())
	{
		if (ImGui::CollapsingHeader("Other components", ImGuiTreeNodeFlags_DefaultOpen))
		{
			for (const std::string& key : others)
			{
				const EngineComponentType* type = FindEngineComponentByKey(key);
				ImGui::BulletText("%s", type ? type->name : ShortCompName(key).c_str());
				// Detachable even with no inline editor: the table knows how to Remove() it.
				//	Removal invalidates `others` for this frame, so stop the loop right after.
				if (type && RemoveEngineComponentButton(scene, selected, *type, history))
					break;
			}
			ImGui::TextDisabled("(present on entity; no inline editor)");
		}
	}

	ImGui::Separator();
	AddComponentButton(scene, selected, history);
}

void PropertiesWindow(Scene& scene, Entity& selected, bool* p_open)
{
	ImGui::SetNextWindowSize(ImVec2(340, 480), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Properties", p_open))
		PropertiesGUI(scene, selected);
	ImGui::End();
}

// ------------------------------------------------------------ entity field ---

bool EntityField(Scene& scene, const char* label, NativeComponent& comp, const char* paramName)
{
	bool changed = false;
	const Entity current = comp.GetEntityRef(paramName);

	// Button face shows the current target (name, or a placeholder when unset / unresolved).
	std::string face;
	if (current != INVALID_ENTITY)
		face = EntityLabel(scene, current);
	else
	{
		// Distinguish "empty" from "set but target missing" (e.g. referenced entity was deleted).
		const std::string guid = comp.GetString(paramName, "");
		face = guid.empty() ? "(none)" : ("[missing] " + guid);
	}

	ImGui::PushID(paramName);

	// The drop target is the button itself; reserve room for the trailing label.
	const float labelW = ImGui::CalcTextSize(label).x + ImGui::GetStyle().ItemInnerSpacing.x;
	ImGui::Button((face + "##entityfield").c_str(), ImVec2(-labelW, 0));

	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(SIMTARY_ENTITY_PAYLOAD))
		{
			Entity dropped = INVALID_ENTITY;
			if (p->DataSize == (int)sizeof(Entity))
				dropped = *(const Entity*)p->Data;
			if (dropped != INVALID_ENTITY)
			{
				comp.SetEntityRef(paramName, dropped); // GUID-based, persisted to metadata
				changed = true;
			}
		}
		ImGui::EndDragDropTarget();
	}

	// Right-click to clear.
	if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && current != INVALID_ENTITY)
	{
		comp.SetEntityRef(paramName, INVALID_ENTITY);
		changed = true;
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Drag an entity here from the Hierarchy.\nRight-click to clear.");

	ImGui::SameLine();
	ImGui::TextUnformatted(label);

	ImGui::PopID();
	return changed;
}
