#include "imhierarchy.h"

#include "stNativeComponent.h"
#include "wiScene.h"
#include "wiMath.h"
#include "imgui.h"

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

static void DrawNode(Scene& scene, Entity e,
	const std::unordered_map<Entity, std::vector<Entity>>& children,
	Entity& selected, std::unordered_set<Entity>& visited)
{
	if (!visited.insert(e).second) return; // cycle guard: never recurse an entity twice

	auto it = children.find(e);
	const bool hasChildren = (it != children.end() && !it->second.empty());

	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (e == selected)   flags |= ImGuiTreeNodeFlags_Selected;
	if (!hasChildren)    flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

	// "##<id>" keeps the visible label clean while giving every row a unique ImGui id.
	const std::string label = EntityLabel(scene, e) + "##" + std::to_string((unsigned)e);
	const bool open = ImGui::TreeNodeEx(label.c_str(), flags);

	// Select on a label click, but ignore the click that only toggled the expand arrow.
	if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
		selected = e;

	// Drag source: lets you drag this entity onto an EntityField (Unity-style object reference).
	if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
	{
		ImGui::SetDragDropPayload(MILISTRY_ENTITY_PAYLOAD, &e, sizeof(Entity));
		ImGui::Text("%s", EntityLabel(scene, e).c_str());
		ImGui::EndDragDropSource();
	}

	if (open && hasChildren)
	{
		for (Entity c : it->second)
			DrawNode(scene, c, children, selected, visited);
		ImGui::TreePop();
	}
}

void HierarchyGUI(Scene& scene, Entity& selected)
{
	static ImGuiTextFilter filter;
	filter.Draw("Filter", -1.0f);

	std::unordered_set<Entity> all;
	GatherEntities(scene, all);

	ImGui::Text("%zu entities", all.size());
	ImGui::SameLine();
	if (ImGui::SmallButton("Deselect"))
		selected = INVALID_ENTITY;
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
			if (ImGui::Selectable(label.c_str(), e == selected))
				selected = e;
			if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
			{
				ImGui::SetDragDropPayload(MILISTRY_ENTITY_PAYLOAD, &e, sizeof(Entity));
				ImGui::Text("%s", EntityLabel(scene, e).c_str());
				ImGui::EndDragDropSource();
			}
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
			DrawNode(scene, e, children, selected, visited);
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

static void DrawTransform(Scene& scene, Entity e)
{
	TransformComponent* t = scene.transforms.GetComponent(e);
	if (!t) return;
	if (!ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) return;

	// Euler is cached per-selection so dragging the rotation field doesn't fight the
	//	quaternion<->euler round-trip (re-extracting every frame would jitter the values).
	static Entity   eulerEntity = INVALID_ENTITY;
	static XMFLOAT3 eulerDeg = XMFLOAT3(0, 0, 0);
	if (eulerEntity != e)
	{
		const XMFLOAT3 rpy = wi::math::QuaternionToRollPitchYaw(t->rotation_local);
		eulerDeg = XMFLOAT3(Deg(rpy.x), Deg(rpy.y), Deg(rpy.z));
		eulerEntity = e;
	}

	bool dirty = false;
	dirty |= ImGui::DragFloat3("Position (local)", &t->translation_local.x, 0.01f);

	if (ImGui::DragFloat3("Rotation (deg)", &eulerDeg.x, 0.5f))
	{
		const XMVECTOR q = XMQuaternionRotationRollPitchYaw(Rad(eulerDeg.x), Rad(eulerDeg.y), Rad(eulerDeg.z));
		XMStoreFloat4(&t->rotation_local, q);
		dirty = true;
	}

	dirty |= ImGui::DragFloat3("Scale", &t->scale_local.x, 0.01f);

	// Simtary 64-bit large-world absolute position.
	double wp[3] = { t->world_translation_x, t->world_translation_y, t->world_translation_z };
	if (ImGui::InputScalarN("World pos (abs)", ImGuiDataType_Double, wp, 3))
		t->SetWorldPosition(wp[0], wp[1], wp[2]); // already flags dirty

	if (dirty)
		t->SetDirty();
}

static void DrawLight(Scene& scene, Entity e)
{
	LightComponent* l = scene.lights.GetComponent(e);
	if (!l) return;
	if (!ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen)) return;

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

static void DrawCamera(Scene& scene, Entity e)
{
	CameraComponent* c = scene.cameras.GetComponent(e);
	if (!c) return;
	if (!ImGui::CollapsingHeader("Camera")) return;

	ImGui::DragFloat("FOV (rad)", &c->fov, 0.01f, 0.01f, XM_PI - 0.01f);
	ImGui::DragFloat("Near", &c->zNearP, 0.01f, 0.001f, 100000.0f);
	ImGui::DragFloat("Far", &c->zFarP, 1.0f, 0.01f, 1000000.0f);
	ImGui::DragFloat("Focal length", &c->focal_length, 0.01f);
	ImGui::DragFloat("Aperture size", &c->aperture_size, 0.01f, 0.0f, 100.0f);
}

static void DrawObject(Scene& scene, Entity e)
{
	ObjectComponent* o = scene.objects.GetComponent(e);
	if (!o) return;
	if (!ImGui::CollapsingHeader("Object", ImGuiTreeNodeFlags_DefaultOpen)) return;

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

static void DrawMaterial(Scene& scene, Entity e)
{
	MaterialComponent* m = scene.materials.GetComponent(e);
	if (!m) return;
	if (!ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) return;

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

static void DrawLayer(Scene& scene, Entity e)
{
	LayerComponent* layer = scene.layers.GetComponent(e);
	if (!layer) return;
	if (!ImGui::CollapsingHeader("Layer")) return;

	int mask = (int)layer->layerMask;
	if (ImGui::InputScalar("Layer mask", ImGuiDataType_S32, &mask, nullptr, nullptr, "%08X", ImGuiInputTextFlags_CharsHexadecimal))
		layer->layerMask = (uint32_t)mask;
}

static void DrawName(Scene& scene, Entity e)
{
	NameComponent* n = scene.names.GetComponent(e);
	if (!n) return;
	EditString("Name", n->name);
}

static void DrawMetadata(Scene& scene, Entity e)
{
	MetadataComponent* md = scene.metadatas.GetComponent(e);
	if (!md) return;
	if (!ImGui::CollapsingHeader("Metadata")) return;

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

static void DrawNativeComponents(Scene& scene, Entity e)
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
		if (ImGui::TreeNodeEx(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
		{
			bool enabled = inst.component->IsEnabled();
			if (ImGui::Checkbox("enabled", &enabled))
				inst.component->SetEnabled(enabled); // writes NCE_<id> metadata (persisted)
			ImGui::SameLine();
			ImGui::TextDisabled(inst.started ? "(started)" : "(awaiting start)");

			ImGui::BeginDisabled(!enabled);
			inst.component->DrawDebug(); // component-supplied widgets bound to live members
			ImGui::EndDisabled();
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
}

void PropertiesGUI(Scene& scene, Entity& selected)
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
	DrawName(scene, selected);
	DrawTransform(scene, selected);
	DrawObject(scene, selected);
	DrawMaterial(scene, selected);
	DrawLight(scene, selected);
	DrawCamera(scene, selected);
	DrawLayer(scene, selected);
	DrawMetadata(scene, selected);

	// Native components.
	DrawNativeComponents(scene, selected);

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
		others.push_back(ShortCompName(kv.first));
	}
	std::sort(others.begin(), others.end());

	if (!others.empty())
	{
		if (ImGui::CollapsingHeader("Other components", ImGuiTreeNodeFlags_DefaultOpen))
		{
			for (const std::string& name : others)
				ImGui::BulletText("%s", name.c_str());
			ImGui::TextDisabled("(present on entity; no inline editor)");
		}
	}
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
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(MILISTRY_ENTITY_PAYLOAD))
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
