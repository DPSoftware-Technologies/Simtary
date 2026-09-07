#include "imcomponents.h"

#include "imeditorhistory.h"
#include "stNativeComponent.h"
#include "wiScene.h"
#include "wiBacklog.h"
#include "imgui.h"

#include <cctype>
#include <cstdio>
#include <string>

using wi::ecs::Entity;
using wi::ecs::INVALID_ENTITY;
using namespace wi::scene;

// ------------------------------------------------------------------ the table ---

// One row per Scene component manager. The lambdas capture nothing, so they decay to
//	plain function pointers and the whole table is a compile-time constant.
#define ST_ENGINE_COMPONENT(DisplayName, Member, Key, NeedsSetup)                 \
	{ DisplayName, Key, NeedsSetup,                                               \
	  [](Scene& s, Entity e) -> bool { return s.Member.Contains(e); },            \
	  [](Scene& s, Entity e) { if (!s.Member.Contains(e)) s.Member.Create(e); },  \
	  [](Scene& s, Entity e) { s.Member.Remove(e); } }

static const EngineComponentType g_engineComponents[] = {
	ST_ENGINE_COMPONENT("Name",               names,              "wi::scene::Scene::names",              false),
	ST_ENGINE_COMPONENT("Layer",              layers,             "wi::scene::Scene::layers",             false),
	ST_ENGINE_COMPONENT("Transform",          transforms,         "wi::scene::Scene::transforms",         false),
	ST_ENGINE_COMPONENT("Hierarchy",          hierarchy,          "wi::scene::Scene::hierarchy",          true),
	ST_ENGINE_COMPONENT("Material",           materials,          "wi::scene::Scene::materials",          false),
	ST_ENGINE_COMPONENT("Mesh",               meshes,             "wi::scene::Scene::meshes",             true),
	ST_ENGINE_COMPONENT("Impostor",           impostors,          "wi::scene::Scene::impostors",          true),
	ST_ENGINE_COMPONENT("Object",             objects,            "wi::scene::Scene::objects",            true),
	ST_ENGINE_COMPONENT("RigidBody Physics",  rigidbodies,        "wi::scene::Scene::rigidbodies",        false),
	ST_ENGINE_COMPONENT("SoftBody Physics",   softbodies,         "wi::scene::Scene::softbodies",         true),
	ST_ENGINE_COMPONENT("Armature",           armatures,          "wi::scene::Scene::armatures",          true),
	ST_ENGINE_COMPONENT("Light",              lights,             "wi::scene::Scene::lights",             false),
	ST_ENGINE_COMPONENT("Camera",             cameras,            "wi::scene::Scene::cameras",            false),
	ST_ENGINE_COMPONENT("Environment Probe",  probes,             "wi::scene::Scene::probes",             false),
	ST_ENGINE_COMPONENT("Force Field",        forces,             "wi::scene::Scene::forces",             false),
	ST_ENGINE_COMPONENT("Decal",              decals,             "wi::scene::Scene::decals",             false),
	ST_ENGINE_COMPONENT("Animation",          animations,         "wi::scene::Scene::animations",         true),
	ST_ENGINE_COMPONENT("Animation Data",     animation_datas,    "wi::scene::Scene::animation_datas",    true),
	ST_ENGINE_COMPONENT("Emitted Particles",  emitters,           "wi::scene::Scene::emitters",           false),
	ST_ENGINE_COMPONENT("Hair Particles",     hairs,              "wi::scene::Scene::hairs",              false),
	ST_ENGINE_COMPONENT("Weather",            weathers,           "wi::scene::Scene::weathers",           false),
	ST_ENGINE_COMPONENT("Sound",              sounds,             "wi::scene::Scene::sounds",             true),
	ST_ENGINE_COMPONENT("Video",              videos,             "wi::scene::Scene::videos",             true),
	ST_ENGINE_COMPONENT("Inverse Kinematics", inverse_kinematics, "wi::scene::Scene::inverse_kinematics", true),
	ST_ENGINE_COMPONENT("Spring",             springs,            "wi::scene::Scene::springs",            false),
	ST_ENGINE_COMPONENT("Collider",           colliders,          "wi::scene::Scene::colliders",          false),
	ST_ENGINE_COMPONENT("Script",             scripts,            "wi::scene::Scene::scripts",            true),
	ST_ENGINE_COMPONENT("Expression",         expressions,        "wi::scene::Scene::expressions",        true),
	ST_ENGINE_COMPONENT("Humanoid",           humanoids,          "wi::scene::Scene::humanoids",          true),
	ST_ENGINE_COMPONENT("Terrain",            terrains,           "wi::scene::Scene::terrains",           true),
	ST_ENGINE_COMPONENT("Sprite",             sprites,            "wi::scene::Scene::sprites",            true),
	ST_ENGINE_COMPONENT("Sprite Font",        fonts,              "wi::scene::Scene::fonts",              true),
	ST_ENGINE_COMPONENT("Voxel Grid",         voxel_grids,        "wi::scene::Scene::voxel_grids",        true),
	ST_ENGINE_COMPONENT("Metadata",           metadatas,          "wi::scene::Scene::metadatas",          false),
	ST_ENGINE_COMPONENT("Character",          characters,         "wi::scene::Scene::characters",         true),
	ST_ENGINE_COMPONENT("Physics Constraint", constraints,        "wi::scene::Scene::constraints",        true),
	ST_ENGINE_COMPONENT("Spline",             splines,            "wi::scene::Scene::splines",            true),
	ST_ENGINE_COMPONENT("Gaussian Splat",     gaussian_splats,    "wi::scene::Scene::gaussian_splats",    true),
};

#undef ST_ENGINE_COMPONENT

const EngineComponentType* EngineComponentTable(size_t& count)
{
	count = IM_ARRAYSIZE(g_engineComponents);
	return g_engineComponents;
}

const EngineComponentType* FindEngineComponentByKey(const std::string& libraryKey)
{
	for (const EngineComponentType& t : g_engineComponents)
	{
		if (libraryKey == t.libraryKey)
			return &t;
	}
	return nullptr;
}

// ------------------------------------------------------------------- add menu ---

// Case-insensitive substring test, so "lig" finds both "Light" and "Spot Light".
static bool MatchesFilter(const char* text, const char* filter)
{
	if (filter == nullptr || filter[0] == 0)
		return true;
	for (const char* a = text; *a != 0; ++a)
	{
		const char* p = a;
		const char* q = filter;
		while (*p != 0 && *q != 0 &&
			std::tolower((unsigned char)*p) == std::tolower((unsigned char)*q))
		{
			++p;
			++q;
		}
		if (*q == 0)
			return true;
	}
	return false;
}

bool AddComponentButton(Scene& scene, Entity entity, st::EditorHistory* history)
{
	if (entity == INVALID_ENTITY)
		return false;

	bool added = false;

	// One search box shared by the popup; cleared every time the popup is opened.
	static char filter[64] = "";

	if (ImGui::Button("+ Add Component", ImVec2(-FLT_MIN, 0)))
	{
		filter[0] = 0;
		ImGui::OpenPopup("##add_component");
	}

	if (!ImGui::BeginPopup("##add_component"))
		return false;

	ImGui::SetNextItemWidth(260);
	if (ImGui::IsWindowAppearing())
		ImGui::SetKeyboardFocusHere();
	ImGui::InputTextWithHint("##filter", "search...", filter, sizeof(filter));
	ImGui::Separator();

	ImGui::BeginChild("##add_component_list", ImVec2(260, 320), false);

	const bool filtering = filter[0] != 0;

	// --- native components: one section per registered group, one sub-tree per category ---
	// "Project" comes back first (a game author reaches for their own components far more
	// often than for the engine's), then "Framework" and anything else a game registered.
	// Only Project is open by default: the whole point of the sections is that the picker
	// opens onto the dozen names that entity is likely to want, not onto all sixty.
	wi::vector<NativeComponentGroup> groups;
	GetRegisteredNativeComponentGroups(groups);

	for (const NativeComponentGroup& group : groups)
	{
		// Count first, so the header can say how many are behind it and a section with no
		// match under the current filter can stay shut instead of opening onto "(no match)".
		int matches = 0;
		for (const std::string& name : group.components)
		{
			if (MatchesFilter(name.c_str(), filter))
				++matches;
		}

		// "[ST] Framework components (12)###group_Framework" - the text before ### is what
		// is drawn, the part after is the ID, so the open/closed state survives the count
		// changing as the filter is typed.
		char header[160];
		if (group.tag.empty())
		{
			snprintf(header, sizeof(header), "%s components (%d)###group_%s",
				group.name.c_str(), matches, group.name.c_str());
		}
		else
		{
			snprintf(header, sizeof(header), "[%s] %s components (%d)###group_%s",
				group.tag.c_str(), group.name.c_str(), matches, group.name.c_str());
		}

		// While something is typed the filter drives the sections: everything with a hit
		// opens, everything without closes. Clearing the box leaves them as the user had
		// them, which is why this is Always only while filtering.
		if (filtering)
			ImGui::SetNextItemOpen(matches > 0, ImGuiCond_Always);
		else
			ImGui::SetNextItemOpen(group.name == NATIVE_COMPONENT_DEFAULT_GROUP, ImGuiCond_FirstUseEver);

		if (!ImGui::CollapsingHeader(header))
			continue;

		if (matches == 0)
		{
			ImGui::TextDisabled(group.components.empty() ? "(none registered)" : "(no match)");
			continue;
		}

		// Inside a group, one sub-tree per category ("Audio", "Optical", ...). The
		// uncategorized bucket comes first and is drawn with no sub-header at all, so a
		// project that never names a category sees a plain list exactly as before.
		for (const NativeComponentCategory& category : group.categories)
		{
			int categoryMatches = 0;
			for (const std::string& name : category.components)
			{
				if (MatchesFilter(name.c_str(), filter))
					++categoryMatches;
			}
			if (categoryMatches == 0)
				continue;

			bool indented = false;
			if (!category.name.empty())
			{
				char label[160];
				snprintf(label, sizeof(label), "%s (%d)###cat_%s_%s",
					category.name.c_str(), categoryMatches,
					group.name.c_str(), category.name.c_str());

				// Open by default - opening a group should show what is in it - but
				// collapsible, so the categories one project never touches can be shut for
				// good. While filtering the hits are forced open, same rule as the groups.
				if (filtering)
					ImGui::SetNextItemOpen(true, ImGuiCond_Always);
				else
					ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);

				if (!ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_SpanAvailWidth))
					continue;
				indented = true;
			}

			for (const std::string& name : category.components)
			{
				if (!MatchesFilter(name.c_str(), filter))
					continue;
				// Native components stack (NCI_0, NCI_1, ...), so there is deliberately no
				// "already attached" gate here the way there is for engine components.
				if (ImGui::Selectable(name.c_str()))
				{
					if (history) history->BeginEntity(scene, entity, "Attach Component");
					const int localID = AttachNativeComponent(scene, entity, name);
					if (history) history->Commit(scene);
					if (localID >= 0)
					{
						wi::backlog::post("Editor: attached native component '" + name +
							"' as NCI_" + std::to_string(localID) +
							" on entity " + std::to_string(entity));
						added = true;
						ImGui::CloseCurrentPopup();
					}
				}
			}

			if (indented)
				ImGui::TreePop();
		}
	}

	// --- engine components ---
	// Closed by default and last: 38 rows that are the same in every project, so they are
	// the part of this popup least worth scrolling past to reach anything else.
	{
		int engineMatches = 0;
		for (const EngineComponentType& t : g_engineComponents)
		{
			if (MatchesFilter(t.name, filter))
				++engineMatches;
		}
		char engineHeader[96];
		snprintf(engineHeader, sizeof(engineHeader), "Engine components (%d)###group_engine", engineMatches);
		if (filtering)
			ImGui::SetNextItemOpen(engineMatches > 0, ImGuiCond_Always);

		if (ImGui::CollapsingHeader(engineHeader))
		{
			// Two passes so the ones that are inert until you feed them data (a Mesh with no
			// vertices, a Terrain with no materials) sit under a warning rather than next to Light.
			for (int pass = 0; pass < 2; ++pass)
			{
				bool wroteSeparator = false;
				for (const EngineComponentType& t : g_engineComponents)
				{
					if (t.needsSetup != (pass == 1))
						continue;
					if (!MatchesFilter(t.name, filter))
						continue;

					if (pass == 1 && !wroteSeparator)
					{
						ImGui::Separator();
						ImGui::TextDisabled("needs data before it does anything");
						wroteSeparator = true;
					}

					const bool present = t.Has(scene, entity);
					ImGui::BeginDisabled(present);
					if (ImGui::Selectable(t.name))
					{
						if (history) history->BeginEntity(scene, entity, "Add Component");
						t.Add(scene, entity);
						if (history) history->Commit(scene);
						wi::backlog::post(std::string("Editor: added engine component '") + t.name +
							"' to entity " + std::to_string(entity));
						added = true;
						ImGui::CloseCurrentPopup();
					}
					ImGui::EndDisabled();
					if (present)
					{
						ImGui::SameLine();
						ImGui::TextDisabled("(on)");
					}
				}
			}
		}
	}

	ImGui::EndChild();
	ImGui::EndPopup();
	return added;
}

// ------------------------------------------------------------------ new object ---

// Give a freshly created or imported entity a home. Component_Attach bakes the world
//	transform into the child's local one, so the object stays exactly where it was put.
void PlaceEntityAt(Scene& scene, Entity e, const XMFLOAT3& position, Entity parent)
{
	if (TransformComponent* t = scene.transforms.GetComponent(e))
	{
		t->translation_local = position;
		// Write the 64-bit ABSOLUTE position too, not just the origin-relative float. That
		//	is the field TransformComponent::Serialize round-trips (it calls SetWorldPosition
		//	on read and rebuilds translation_local from it), so an object placed only in local
		//	space comes back at the world origin after a save/load - or after an undo/redo.
		t->SyncWorldFromLocal(wi::scene::GetRenderOrigin());
		t->SetDirty();
		t->UpdateTransform();
	}
	if (parent != INVALID_ENTITY && parent != e && scene.transforms.Contains(parent))
		scene.Component_Attach(e, parent);
}

namespace {

// Unique-ish display name, so ten cubes are not all called "Cube".
std::string UniqueName(Scene& scene, const char* base)
{
	if (scene.Entity_FindByName(base) == INVALID_ENTITY)
		return base;
	for (int i = 1; i < 10000; ++i)
	{
		const std::string candidate = std::string(base) + " " + std::to_string(i);
		if (scene.Entity_FindByName(candidate) == INVALID_ENTITY)
			return candidate;
	}
	return base;
}

} // namespace

Entity CreateObjectMenuItems(Scene& scene, const XMFLOAT3& spawnPosition, Entity parent,
	st::EditorHistory* history)
{
	Entity created = INVALID_ENTITY;
	const char* label = nullptr;

	if (ImGui::MenuItem("Empty"))
	{
		created = scene.Entity_CreateTransform(UniqueName(scene, "Empty"));
		label = "Create Empty";
	}

	ImGui::Separator();
	if (ImGui::MenuItem("Cube"))
	{
		created = scene.Entity_CreateCube(UniqueName(scene, "Cube"));
		label = "Create Cube";
	}
	if (ImGui::MenuItem("Sphere"))
	{
		created = scene.Entity_CreateSphere(UniqueName(scene, "Sphere"));
		label = "Create Sphere";
	}
	if (ImGui::MenuItem("Plane"))
	{
		created = scene.Entity_CreatePlane(UniqueName(scene, "Plane"));
		label = "Create Plane";
	}

	ImGui::Separator();
	if (ImGui::BeginMenu("Light"))
	{
		if (ImGui::MenuItem("Directional"))
		{
			created = scene.Entity_CreateLight(UniqueName(scene, "Directional Light"), spawnPosition,
				XMFLOAT3(1, 1, 1), 10.0f, 1000.0f, LightComponent::DIRECTIONAL);
			label = "Create Directional Light";
		}
		if (ImGui::MenuItem("Point"))
		{
			created = scene.Entity_CreateLight(UniqueName(scene, "Point Light"), spawnPosition,
				XMFLOAT3(1, 1, 1), 20.0f, 10.0f, LightComponent::POINT);
			label = "Create Point Light";
		}
		if (ImGui::MenuItem("Spot"))
		{
			created = scene.Entity_CreateLight(UniqueName(scene, "Spot Light"), spawnPosition,
				XMFLOAT3(1, 1, 1), 20.0f, 20.0f, LightComponent::SPOT);
			label = "Create Spot Light";
		}
		ImGui::EndMenu();
	}

	if (ImGui::MenuItem("Camera"))
	{
		// Sized from the main viewport; the render path overwrites width/height anyway if
		// this camera is ever made active.
		created = scene.Entity_CreateCamera(UniqueName(scene, "Camera"), 1920.0f, 1080.0f);
		label = "Create Camera";
	}

	ImGui::Separator();
	if (ImGui::MenuItem("Environment Probe"))
	{
		created = scene.Entity_CreateEnvironmentProbe(UniqueName(scene, "EnvProbe"), spawnPosition);
		label = "Create Environment Probe";
	}
	if (ImGui::MenuItem("Force Field"))
	{
		created = scene.Entity_CreateForce(UniqueName(scene, "Force Field"), spawnPosition);
		label = "Create Force Field";
	}
	if (ImGui::MenuItem("Emitted Particles"))
	{
		created = scene.Entity_CreateEmitter(UniqueName(scene, "Emitter"), spawnPosition);
		label = "Create Emitter";
	}
	if (ImGui::MenuItem("Hair Particles"))
	{
		created = scene.Entity_CreateHair(UniqueName(scene, "Hair"), spawnPosition);
		label = "Create Hair";
	}
	if (ImGui::MenuItem("Decal"))
	{
		// No texture yet - the inspector's Material/Decal fields are where one gets set.
		created = scene.Entity_CreateDecal(UniqueName(scene, "Decal"), "");
		label = "Create Decal";
	}
	if (ImGui::MenuItem("Weather"))
	{
		created = scene.Entity_CreateTransform(UniqueName(scene, "Weather"));
		scene.weathers.Create(created);
		label = "Create Weather";
	}

	if (created == INVALID_ENTITY)
		return INVALID_ENTITY;

	PlaceEntityAt(scene, created, spawnPosition, parent);
	if (history != nullptr)
		history->RecordCreated(scene, created, label);
	wi::backlog::post(std::string("Editor: ") + label + " (entity " + std::to_string(created) + ")");
	return created;
}

bool RemoveEngineComponentButton(Scene& scene, Entity entity, const EngineComponentType& type,
	st::EditorHistory* history)
{
	bool removed = false;

	ImGui::PushID(type.libraryKey);
	// Right-aligned, so a component row reads "Header ................ x".
	ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
	if (ImGui::SmallButton("x"))
	{
		if (history) history->BeginEntity(scene, entity, "Remove Component");
		type.Remove(scene, entity);
		if (history) history->Commit(scene);
		wi::backlog::post(std::string("Editor: removed engine component '") + type.name +
			"' from entity " + std::to_string(entity));
		removed = true;
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Remove %s from this entity", type.name);
	ImGui::PopID();

	return removed;
}
