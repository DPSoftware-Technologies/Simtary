#include "imeditor.h"

#include "io/asset/AssetSystem.h"
#include "io/model/ModelImporter.h"
#include "io/asset/SceneDescriptor.h"
#include "io/asset/AssetPackWriter.h"

#include "stApp.h"
#include "imhierarchy.h"
#include "imcomponents.h"
#include "imassets.h"
#include "imgraphicsettings.h"
#include "input/InputSystem.h"

#include "wiScene.h"
#include "wiRenderer.h"
#include "wiArchive.h"
#include "wiHelper.h"
#include "wiBacklog.h"
#include "wiEnums.h"
#include "wiMath.h"

#include "imgui_internal.h"   // DockBuilder*: authoring the default layout

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using wi::ecs::Entity;
using wi::ecs::INVALID_ENTITY;
using wi::scene::CameraComponent;
using wi::scene::HierarchyComponent;
// NOTE: no `using wi::scene::Scene` here — Framework/stScene.h declares a global `Scene`
// (the game-facing scene base class), and the two names would collide.
using wi::scene::TransformComponent;

namespace {

// Window titles. They are the dock-node keys too, so they must match what
// BuildDefaultLayout() hands to DockBuilderDockWindow.
constexpr const char* kEditorViewport = "Editor Viewport";
constexpr const char* kGameViewport   = "Game Viewport";
constexpr const char* kHierarchy      = "Hierarchy##editor";
constexpr const char* kProperties     = "Properties##editor";
constexpr const char* kImportOptions  = "Import Options##editor";
// The "##editor" suffix keeps this a DIFFERENT ImGui window from the floating DevUI
// copy, so docking one does not move or close the other. Both drive the same
// AssetExplorer object through App::Resources().
constexpr const char* kResources      = "Resource Explorer##editor";
constexpr const char* kDockHost       = "##SimtaryEditorDockHost";

constexpr float kPitchLimit = 1.55f; // ~89 degrees; stops the freecam flipping over the pole

// Aspect presets. Index 0 is "free": width and height move independently. Shared by the
//	Game Viewport's resolution override and by every Camera View.
struct AspectRatio { const char* label; float value; };
const AspectRatio kAspectRatios[] = {
	{ "Free",    0.0f },
	{ "16:9",    16.0f / 9.0f },
	{ "16:10",   16.0f / 10.0f },
	{ "21:9",    21.0f / 9.0f },
	{ "2.39:1",  2.39f },
	{ "4:3",     4.0f / 3.0f },
	{ "3:2",     3.0f / 2.0f },
	{ "1:1",     1.0f },
	{ "9:16",    9.0f / 16.0f },
};

// The "Resolution" + "Ratio" menu pair. One copy, two callers: a Camera View's authored
//	output size and the Game Viewport's resolution override are the same control.
//	`ratio` locks the aspect, so Width drives Height and Height goes read-only.
void DrawResolutionMenus(int& width, int& height, int& ratio)
{
	if (ImGui::BeginMenu("Resolution"))
	{
		struct Preset { const char* label; int w, h; };
		static const Preset presets[] = {
			{ "1280 x 720",   1280, 720 },
			{ "1920 x 1080",  1920, 1080 },
			{ "2560 x 1440",  2560, 1440 },
			{ "3840 x 2160",  3840, 2160 },
			{ "1080 x 1920",  1080, 1920 },
			{ "1080 x 1080",  1080, 1080 },
			{ "2048 x 858",   2048, 858 },
		};
		for (const Preset& p : presets)
		{
			const bool active = (width == p.w && height == p.h);
			if (ImGui::MenuItem(p.label, nullptr, active))
			{
				width = p.w;
				height = p.h;
				ratio = 0; // an explicit resolution wins over a locked ratio
			}
		}
		ImGui::Separator();
		ImGui::SetNextItemWidth(90);
		if (ImGui::DragInt("Width", &width, 4, 16, 8192) && ratio > 0)
			height = std::max(16, (int)(width / kAspectRatios[ratio].value));
		ImGui::SetNextItemWidth(90);
		ImGui::BeginDisabled(ratio > 0); // height is derived while a ratio is locked
		ImGui::DragInt("Height", &height, 4, 16, 8192);
		ImGui::EndDisabled();
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Ratio"))
	{
		for (int i = 0; i < IM_ARRAYSIZE(kAspectRatios); ++i)
		{
			if (!ImGui::MenuItem(kAspectRatios[i].label, nullptr, ratio == i))
				continue;
			ratio = i;
			if (i > 0)
				height = std::max(16, (int)(width / kAspectRatios[i].value));
		}
		ImGui::EndMenu();
	}
}

// "Show Game Preview" off: RenderPath3D's own defaults, which is exactly what an editor
//	path looked like before there was a preview at all. Spelled out rather than left to the
//	constructor because the path object is REUSED - turning the preview off has to undo what
//	GraphicsSettings::MirrorTo wrote into it, or the game's tonemap and colour grading simply
//	stay. Kept in the same order as MirrorTo so the two can be diffed by eye.
void ApplyEngineDefaults(wi::RenderPath3D& path)
{
	// The projector / laser passes were copied off the game path; they go with the preview.
	path.custom_post_processes.clear();
	path.setlayerMask(0xFFFFFFFF);
	path.colorspace = wi::graphics::ColorSpace::SRGB;

	path.setAO(wi::RenderPath3D::AO_DISABLED);
	path.setAORange(1.0f);
	path.setAOSampleCount(16);
	path.setAOPower(1.0f);
	path.setSSREnabled(false);
	path.setSSRQuality(wi::renderer::PostProcessQuality::Medium);
	path.setSSGIEnabled(false);
	path.setSSGIDepthRejection(8.0f);
	path.setRaytracedReflectionsEnabled(false);
	path.setRaytracedReflectionsRange(10000.0f);
	path.setRaytracedReflectionsQuality(wi::renderer::PostProcessQuality::Medium);
	path.setReflectionRoughnessCutoff(0.6f);
	path.setRaytracedDiffuseEnabled(false);
	path.setRaytracedDiffuseRange(10.0f);
	path.setRaytracedDiffuseQuality(wi::renderer::PostProcessQuality::Medium);
	path.setReflectionsEnabled(true);

	path.setShadowsEnabled(true);
	path.setScreenSpaceShadowSampleCount(16);
	path.setScreenSpaceShadowRange(1.0f);

	path.setFXAAEnabled(false);
	path.setBloomEnabled(true);
	path.setBloomThreshold(1.0f);
	path.setMotionBlurEnabled(false);
	path.setMotionBlurStrength(100.0f);
	path.setDepthOfFieldEnabled(true);
	path.setDepthOfFieldStrength(10.0f);
	path.setSharpenFilterEnabled(false);
	path.setSharpenFilterAmount(0.28f);
	path.setCRTFilterEnabled(false);
	path.setLensFlareEnabled(true);
	path.setLightShaftsEnabled(false);
	path.setLightShaftsStrength(0.5f);
	path.setLightShaftsFadeSpeed(3.0f);
	path.setVolumeLightsEnabled(true);
	path.setChromaticAberrationEnabled(false);
	path.setChromaticAberrationAmount(2.0f);
	path.setDitherEnabled(true);
	path.setOutlineEnabled(false);
	path.setOutlineThreshold(0.2f);
	path.setOutlineThickness(1.0f);
	path.setOutlineColor(XMFLOAT4(0, 0, 0, 1));

	path.setEyeAdaptionEnabled(false);
	path.setEyeAdaptionKey(0.115f);
	path.setEyeAdaptionRate(1.0f);

	path.setTonemap(wi::renderer::Tonemap::ACES);
	path.setColorGradingEnabled(true);
	path.setExposure(1.0f);
	path.setHDRCalibration(1.0f);
	path.setBrightness(0.0f);
	path.setContrast(1.0f);
	path.setSaturation(1.0f);

	path.setMeshBlendEnabled(true);
	path.setVisibilityComputeShadingEnabled(false);
	path.setMSAASampleCount(1);
}

// Wireframe frustum for a camera that has no CameraComponent in the scene — the ACTIVE game
//	camera is a free-standing wi::scene::GetCamera(), so wi::renderer's own "debug cameras"
//	pass (which walks scene.cameras) never draws it, and it is the one you most want to see
//	from the editor viewport.
//
//	Queued, not drawn: DrawDebugWorld consumes the queue, and the game path has already
//	rendered by the time this runs, so only the editor viewport picks these up.
void QueueCameraFrustum(const CameraComponent& cam, const XMFLOAT4& color)
{
	const XMMATRIX invVP = XMLoadFloat4x4(&cam.InvVP);

	// NDC corners. The engine uses a reverse-Z projection, so the NEAR plane is z=1.
	static const XMFLOAT3 ndc[8] = {
		XMFLOAT3(-1, -1, 1), XMFLOAT3(1, -1, 1), XMFLOAT3(1, 1, 1), XMFLOAT3(-1, 1, 1), // near
		XMFLOAT3(-1, -1, 0), XMFLOAT3(1, -1, 0), XMFLOAT3(1, 1, 0), XMFLOAT3(-1, 1, 0), // far
	};
	XMFLOAT3 corner[8];
	for (int i = 0; i < 8; ++i)
		XMStoreFloat3(&corner[i], XMVector3TransformCoord(XMLoadFloat3(&ndc[i]), invVP));

	auto edge = [&](int a, int b) {
		wi::renderer::RenderableLine line;
		line.start = corner[a];
		line.end = corner[b];
		line.color_start = color;
		line.color_end = color;
		wi::renderer::DrawLine(line);
	};
	for (int i = 0; i < 4; ++i)
	{
		edge(i, (i + 1) % 4);             // near rectangle
		edge(4 + i, 4 + ((i + 1) % 4));   // far rectangle
		edge(i, 4 + i);                   // the four side edges
	}
}

// Wireframe box around one entity, taken from whichever bounds array the scene already keeps
//	for it. Returns false when the entity has no bounds at all -- an empty transform, a bare
//	light, a material -- so the caller can fall back to something else.
bool QueueEntityBounds(wi::scene::Scene& scene, Entity e, const XMFLOAT4& color)
{
	auto box = [&](const wi::vector<wi::primitive::AABB>& bounds, size_t index) {
		if (index >= bounds.size())
			return false;
		// depth = false: no depth test, so a selection standing behind or inside other geometry
		//	is still visible. Finding what the Hierarchy just selected is the whole point.
		wi::renderer::DrawBox(bounds[index], color, false);
		return true;
	};

	if (scene.objects.Contains(e) && box(scene.aabb_objects, scene.objects.GetIndex(e))) return true;
	if (scene.lights.Contains(e)  && box(scene.aabb_lights,  scene.lights.GetIndex(e)))  return true;
	if (scene.decals.Contains(e)  && box(scene.aabb_decals,  scene.decals.GetIndex(e)))  return true;
	if (scene.probes.Contains(e)  && box(scene.aabb_probes,  scene.probes.GetIndex(e)))  return true;
	return false;
}

// Show what is selected in the PICTURE, not only in the Properties panel.
//
//	The whole SUBTREE is drawn, not just the selected entity: an imported model's root carries
//	no ObjectComponent at all -- the meshes hang off its children -- so outlining the entity
//	alone would leave the most common selection in this editor with no highlight whatsoever.
//	The entity's own bounds are drawn bright and its descendants' dimmed, so selecting a parent
//	still reads differently from selecting one of its parts.
//
//	An entity with no bounds anywhere in its subtree (a bare light, a camera, an empty group)
//	falls back to a small box at its world position, which is enough to say where it is.
//
//	Queued rather than drawn, exactly like the camera frustum above: DrawDebugWorld consumes
//	the queue, and the game path has already rendered by the time this runs, so only the editor
//	viewport ever shows it.
void QueueSelectionHighlight(wi::scene::Scene& scene, Entity selected)
{
	if (selected == INVALID_ENTITY)
		return;

	const XMFLOAT4 selfColor  = XMFLOAT4(1.00f, 0.62f, 0.12f, 1.00f);
	// A dimmer orange rather than a lower alpha: the debug cube pipeline does not alpha blend,
	//	so fading the colour is the only way to make the children read as secondary.
	const XMFLOAT4 childColor = XMFLOAT4(0.55f, 0.34f, 0.07f, 1.00f);

	bool drawn = QueueEntityBounds(scene, selected, selfColor);

	// One pass over the hierarchy links, then a walk down from the selection. Building the map
	//	is O(links); re-scanning for children at every level instead is O(links x depth), every
	//	frame, on a list that can hold every entity in the map.
	std::unordered_map<Entity, std::vector<Entity>> children;
	for (size_t i = 0; i < scene.hierarchy.GetCount(); ++i)
		children[scene.hierarchy[i].parentID].push_back(scene.hierarchy.GetEntity(i));

	std::vector<Entity> stack{ selected };
	std::unordered_set<Entity> visited;
	while (!stack.empty())
	{
		const Entity e = stack.back();
		stack.pop_back();
		if (!visited.insert(e).second) // cycle guard: a corrupt parent link cannot spin here
			continue;
		if (e != selected && QueueEntityBounds(scene, e, childColor))
			drawn = true;
		const auto it = children.find(e);
		if (it != children.end())
			stack.insert(stack.end(), it->second.begin(), it->second.end());
	}

	if (!drawn)
	{
		if (const TransformComponent* t = scene.transforms.GetComponent(selected))
		{
			wi::primitive::AABB marker;
			marker.createFromHalfWidth(t->GetPosition(), XMFLOAT3(0.25f, 0.25f, 0.25f));
			wi::renderer::DrawBox(marker, selfColor, false);
		}
	}
}

// Swap wi::renderer's global debug switches in for the duration of one render, then put the
//	originals back. That is what makes the debug draws editor-viewport-only.
struct DebugFlagScope
{
	bool cameras, colliders, emitters, probes, forces, springs, bones, partition, grid, voxels;

	DebugFlagScope()
	{
		cameras   = wi::renderer::GetToDrawDebugCameras();
		colliders = wi::renderer::GetToDrawDebugColliders();
		emitters  = wi::renderer::GetToDrawDebugEmitters();
		probes    = wi::renderer::GetToDrawDebugEnvProbes();
		forces    = wi::renderer::GetToDrawDebugForceFields();
		springs   = wi::renderer::GetToDrawDebugSprings();
		bones     = wi::renderer::GetToDrawDebugBoneLines();
		partition = wi::renderer::GetToDrawDebugPartitionTree();
		grid      = wi::renderer::GetToDrawGridHelper();
		voxels    = wi::renderer::GetToDrawVoxelHelper();
	}
	~DebugFlagScope()
	{
		wi::renderer::SetToDrawDebugCameras(cameras);
		wi::renderer::SetToDrawDebugColliders(colliders);
		wi::renderer::SetToDrawDebugEmitters(emitters);
		wi::renderer::SetToDrawDebugEnvProbes(probes);
		wi::renderer::SetToDrawDebugForceFields(forces);
		wi::renderer::SetToDrawDebugSprings(springs);
		wi::renderer::SetToDrawDebugBoneLines(bones);
		wi::renderer::SetToDrawDebugPartitionTree(partition);
		wi::renderer::SetToDrawGridHelper(grid);
		wi::renderer::SetToDrawVoxelHelper(voxels, 1);
	}
};

} // namespace

// ------------------------------------------------------------------ lifecycle ---

void st::EditorUI::SetEnabled(bool on)
{
	if (on == enabled_)
		return;
	enabled_ = on;

	if (enabled_)
	{
		EnsureEditorPath();
	}
	else
	{
		// Back to the window canvas before anything else: this is the one path a shipping
		//	frame takes out of editor mode, and it must not leave the game at 1080p in a
		//	4K window.
		ApplyGameViewResolution(false);

		// The render targets may still be referenced by frames in flight.
		DestroyCameraViews();
		if (editorPath_)
		{
			wi::graphics::GetDevice()->WaitForGPU();
			editorPath_.reset();
		}
		editorViewLive_ = false;
		ReleaseFreeCamLook();
		ReleaseInputCapture();
	}
}

void st::EditorUI::ReleaseInput()
{
	ReleaseFreeCamLook();
	ReleaseInputCapture();
	// Draw() is what re-applies the Game Viewport's resolution override every frame, and
	//	hiding the DevUI is exactly the case where Draw() stops running. Same reason the
	//	input capture is handed back here.
	ApplyGameViewResolution(false);
}

void st::EditorUI::Shutdown()
{
	ReleaseFreeCamLook();
	ReleaseInputCapture();
	ApplyGameViewResolution(false);
	DestroyCameraViews();
	if (editorPath_)
	{
		wi::graphics::GetDevice()->WaitForGPU();
		editorPath_.reset();
	}
	enabled_ = false;
}

void st::EditorUI::SnapCameraToGameCamera()
{
	// Rebuild the euler pair from the game camera's forward vector: with
	//	RotateRollPitchYaw the forward is (sin y * cos x, -sin x, cos y * cos x).
	const CameraComponent& gc = wi::scene::GetCamera();
	XMFLOAT3 f;
	XMStoreFloat3(&f, XMVector3Normalize(XMLoadFloat3(&gc.At)));
	camPos_   = gc.Eye;
	camRot_.y = std::atan2(f.x, f.z);
	camRot_.x = -std::asin(std::clamp(f.y, -1.0f, 1.0f));
	camRot_.z = 0.0f;
	if (gc.fov > 0.0f)
		camFov_ = gc.fov;
}

XMFLOAT3 st::EditorUI::SpawnPoint() const
{
	// Six metres down the free camera's forward axis: far enough not to be inside the near
	//	plane, close enough that a new object is on screen the moment it appears.
	const XMVECTOR q = XMQuaternionRotationRollPitchYaw(camRot_.x, camRot_.y, camRot_.z);
	const XMVECTOR forward = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), q);
	XMFLOAT3 p;
	XMStoreFloat3(&p, XMVectorAdd(XMLoadFloat3(&camPos_), XMVectorScale(forward, 6.0f)));
	return p;
}

const char* st::EditorUI::GizmoLabel() const
{
	switch (gizmoOp_)
	{
	case ImGuizmo::ROTATE: return "Rotate";
	case ImGuizmo::SCALE:  return "Scale";
	default:               return "Move";
	}
}

void st::EditorUI::EnsureEditorPath()
{
	if (editorPath_)
		return;

	// Open looking at whatever the game camera was looking at, not at the world origin —
	//	the default (0,3,-8) is usually inside the floor. Only the first time, so toggling
	//	editor mode off and on again keeps the camera the user left it at.
	if (!camInitialized_)
	{
		SnapCameraToGameCamera();
		camInitialized_ = true;
	}

	editorPath_ = std::make_unique<wi::RenderPath3D>();

	// Its own camera: sharing the game camera would make the two paths fight over
	// CameraComponent::width/height (RenderPath3D::ResizeBuffers writes them).
	editorCamera_.CreatePerspective(1280, 720, camNear_, camFar_, camFov_);
	editorPath_->camera = &editorCamera_;

	// Same contract as the game path: scenes own Scene::Update, this must not run it a
	// second time (see st::App::Initialize for why that corrupts skinning velocity).
	editorPath_->setSceneUpdateEnabled(false);
	// Occlusion culling keeps per-path GPU query history; a viewport that is only
	// sometimes rendered would feed it stale results, so leave it off here.
	editorPath_->setOcclusionCullingEnabled(false);
	editorPath_->setMSAASampleCount(1);

	editorPath_->init(1280, 720, 96.0f);
	editorPath_->Start(); // allocates the render targets (ActivatePath would normally do this)

}

// ------------------------------------------------------------------- dock host ---

void st::EditorUI::BuildDefaultLayout(ImGuiID dockspaceID, ImVec2 size)
{
	ImGui::DockBuilderRemoveNode(dockspaceID);
	ImGui::DockBuilderAddNode(dockspaceID, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(dockspaceID, size);

	// Right column (26%), split again so Hierarchy sits above Properties.
	ImGuiID centre = dockspaceID;
	ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.26f, nullptr, &centre);
	ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.60f, nullptr, &right);

	// A bottom strip across the centre, the shape every engine gives a content browser:
	// wide, short, and under the thing it feeds.
	ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.30f, nullptr, &centre);

	ImGui::DockBuilderDockWindow(kHierarchy, right);
	ImGui::DockBuilderDockWindow(kProperties, rightBottom);
	ImGui::DockBuilderDockWindow(kResources, bottom);
	// Both viewports land in the centre node, so they come up as tabs and either can be
	// pulled out into a split by hand.
	ImGui::DockBuilderDockWindow(kGameViewport, centre);
	ImGui::DockBuilderDockWindow(kEditorViewport, centre);

	ImGui::DockBuilderFinish(dockspaceID);
}

void st::EditorUI::DrawDockHost(App& app, wi::scene::Scene& scene)
{
	const ImGuiViewport* vp = ImGui::GetMainViewport();

	// WorkPos/WorkSize already exclude the DevUI main menu bar, which was submitted
	// earlier this frame.
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(vp->WorkSize);
	ImGui::SetNextWindowViewport(vp->ID);

	const ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
		ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_MenuBar;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::Begin(kDockHost, nullptr, flags);
	ImGui::PopStyleVar(3);

	// ── menu bar ─────────────────────────────────────────────────────────────
	if (ImGui::BeginMenuBar())
	{
		if (ImGui::BeginMenu("Scene"))
		{
			const bool hasPath = !scenePath_.empty();
			if (ImGui::MenuItem("Save", "Ctrl+S", false, hasPath))
				SaveScene(scene, scenePath_);
			if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S"))
				RequestSaveAs("stsd");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Native .stsd: entities here, textures in the asset package");
			ImGui::Separator();
			if (ImGui::MenuItem("Export .wiscene..."))
				RequestSaveAs("wiscene");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("One self-contained wi::Archive, for the standalone Wicked editor");
			ImGui::Separator();
			if (ImGui::MenuItem("Import model...", nullptr, false, !importDialogOpen_))
				RequestImportModel();
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s\n\nMerge one into THIS scene, in front of the editor "
					"camera. Dropping one on the window does the same.",
					ImportFilterDescription().c_str());
			ImGui::MenuItem("Ask for import options", nullptr, &importAskEveryTime_);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("The panel that offers grouping, placement, animations, "
					"skins and scale before an import runs.");
			ImGui::Separator();
			ImGui::TextDisabled("%s", hasPath ? scenePath_.c_str() : "(never saved)");
			if (!lastImportMessage_.empty())
				ImGui::TextDisabled("%s", lastImportMessage_.c_str());
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Create"))
		{
			// New objects land in front of the free camera, parented under the selection when
			//	there is one — the usual "build where I am looking" behaviour.
			const Entity created = CreateObjectMenuItems(scene, SpawnPoint(), INVALID_ENTITY, &history_);
			if (created != INVALID_ENTITY)
				pendingSelection_ = created;
			ImGui::Separator();
			// Same spawn point, from a file instead of from the primitive list.
			if (ImGui::MenuItem("Import model...", nullptr, false, !importDialogOpen_))
				RequestImportModel();
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Edit"))
		{
			const std::string undoLabel = history_.UndoLabel();
			const std::string redoLabel = history_.RedoLabel();
			if (ImGui::MenuItem(undoLabel.empty() ? "Undo" : ("Undo " + undoLabel).c_str(),
					"Ctrl+Z", false, history_.CanUndo()))
				pendingSelection_ = history_.Undo(scene);
			if (ImGui::MenuItem(redoLabel.empty() ? "Redo" : ("Redo " + redoLabel).c_str(),
					"Ctrl+Y", false, history_.CanRedo()))
				pendingSelection_ = history_.Redo(scene);
			ImGui::Separator();
			if (ImGui::MenuItem("Clear History"))
				history_.Clear();
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("View"))
		{
			ImGui::MenuItem("Editor Viewport", nullptr, &showEditorViewport_);
			ImGui::MenuItem("Game Viewport", nullptr, &showGameViewport_);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Its menu bar sets the resolution the GAME renders at.");
			ImGui::Separator();
			if (ImGui::MenuItem("New Camera View"))
			{
				// Start it on the selection when that is already a camera; otherwise it opens
				//	empty with a camera picker, which is the honest thing to show.
				const Entity seed = scene.cameras.Contains(pendingSelection_)
					? pendingSelection_ : INVALID_ENTITY;
				SpawnCameraView(seed);
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("A view-only window on any camera, at any resolution and "
					"aspect. Open as many as you like.");
			ImGui::BeginDisabled(cameraViews_.empty());
			if (ImGui::MenuItem("Close All Camera Views"))
			{
				for (auto& v : cameraViews_)
					v->open = false;
			}
			ImGui::EndDisabled();
			ImGui::Separator();
			ImGui::MenuItem("Hierarchy", nullptr, &showHierarchy_);
			ImGui::MenuItem("Properties", nullptr, &showProperties_);
			ImGui::Separator();
			if (ImGui::MenuItem("Reset Layout"))
				resetLayout_ = true;
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Debug"))
		{
			// The preview switch itself lives on each viewport's own menu bar - it is a
			//	per-viewport setting, and this is the global knob that goes with it.
			ImGui::MenuItem("Show Game Preview (Editor Viewport)", nullptr, &showGamePreview_);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Also on the Editor Viewport's own menu bar. Each Camera "
					"View carries its own copy.");
			ImGui::MenuItem("Stable exposure", nullptr, &stableExposure_);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Applies to every previewing viewport. Pins the exposure "
					"instead of mirroring auto-exposure: an auto-exposure history is "
					"per-path, so a view that only renders while its panel is visible drifts "
					"to its own brightness and stops matching.");
			ImGui::Separator();

			ImGui::TextDisabled("debug draws - editor viewport only");
			ImGui::Separator();
			ImGui::MenuItem("Cameras", nullptr, &debug_.cameras);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Wireframe frustum for every Camera component in the scene");
			ImGui::MenuItem("Active game camera", nullptr, &debug_.gameCamera);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("The camera the game renders through. It is a standalone wi::scene::GetCamera(), not a scene component, so the engine's own camera debug pass never draws it.");
			ImGui::Separator();
			ImGui::MenuItem("Colliders", nullptr, &debug_.colliders);
			ImGui::MenuItem("Emitters", nullptr, &debug_.emitters);
			ImGui::MenuItem("Env probes", nullptr, &debug_.envProbes);
			ImGui::MenuItem("Force fields", nullptr, &debug_.forceFields);
			ImGui::MenuItem("Springs", nullptr, &debug_.springs);
			ImGui::MenuItem("Bone lines", nullptr, &debug_.boneLines);
			ImGui::MenuItem("Partition tree", nullptr, &debug_.partitionTree);
			ImGui::Separator();
			ImGui::MenuItem("Selection highlight", nullptr, &debug_.selection);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Wireframe box around the selected entity and, dimmed, around every "
					"part under it. An imported model's root has no bounds of its own, which is why "
					"the subtree is drawn and not just the row you clicked.");
			ImGui::Separator();
			ImGui::MenuItem("Grid", nullptr, &debug_.grid);
			ImGui::MenuItem("Voxel helper", nullptr, &debug_.voxels);
			ImGui::BeginDisabled(!debug_.voxels);
			ImGui::SetNextItemWidth(90);
			ImGui::DragInt("clipmap", &debug_.voxelClipmap, 1, 0, 7);
			ImGui::EndDisabled();
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Camera"))
		{
			ImGui::TextDisabled("hold RMB to look, WASD/QE to fly");
			ImGui::DragFloat("Move speed", &camSpeed_, 0.1f, 0.1f, 2000.0f);
			ImGui::DragFloat("Boost x", &camBoost_, 0.1f, 1.0f, 50.0f);
			ImGui::DragFloat("Sensitivity", &camSens_, 0.0001f, 0.0005f, 0.05f, "%.4f");

			float fovDeg = camFov_ * (180.0f / XM_PI);
			if (ImGui::DragFloat("FOV (deg)", &fovDeg, 0.5f, 10.0f, 150.0f))
				camFov_ = fovDeg * (XM_PI / 180.0f);

			ImGui::DragFloat("Near", &camNear_, 0.01f, 0.001f, 100.0f);
			ImGui::DragFloat("Far", &camFar_, 10.0f, 1.0f, 1000000.0f);

			if (ImGui::MenuItem("Snap to game camera"))
				SnapCameraToGameCamera();
			ImGui::EndMenu();
		}

		DrawToolbar();

		// Right-aligned status: which scene is live and whether it has a file yet.
		{
			const std::string status = app.sceneManager.CurrentName() +
				(scenePath_.empty() ? "  (unsaved)" : "");
			const float w = ImGui::CalcTextSize(status.c_str()).x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
			if (ImGui::GetCursorPosX() < ImGui::GetWindowWidth() - w)
				ImGui::SetCursorPosX(ImGui::GetWindowWidth() - w);
			ImGui::TextDisabled("%s", status.c_str());
		}

		ImGui::EndMenuBar();
	}

	// ── the dockspace ────────────────────────────────────────────────────────
	const ImGuiID dockspaceID = ImGui::GetID("SimtaryEditorDockSpace");

	// Author the layout only when there is nothing to restore (fresh imgui.ini) or the
	// user asked for a reset — otherwise their arrangement would be wiped every launch.
	if (resetLayout_ || (!layoutBuilt_ && ImGui::DockBuilderGetNode(dockspaceID) == nullptr))
	{
		BuildDefaultLayout(dockspaceID, vp->WorkSize);
		resetLayout_ = false;
	}
	layoutBuilt_ = true;

	ImGui::DockSpace(dockspaceID, ImVec2(0, 0), ImGuiDockNodeFlags_None);
	ImGui::End();
}

void st::EditorUI::DrawToolbar()
{
	ImGui::Separator();

	// Keyboard shortcuts, Unity-ish. Suppressed while a text field has the keyboard, while
	// the right mouse button is down (that is the freecam, where W/E are fly keys), and while
	// the Game Viewport has focus — there W/E/R belong to whatever the game does with them.
	if (!ImGui::GetIO().WantTextInput && !ImGui::IsMouseDown(ImGuiMouseButton_Right)
		&& !gameViewFocused_)
	{
		if (ImGui::IsKeyPressed(ImGuiKey_W, false)) gizmoOp_ = ImGuizmo::TRANSLATE;
		if (ImGui::IsKeyPressed(ImGuiKey_E, false)) gizmoOp_ = ImGuizmo::ROTATE;
		if (ImGui::IsKeyPressed(ImGuiKey_R, false)) gizmoOp_ = ImGuizmo::SCALE;
	}

	auto opButton = [&](const char* label, ImGuizmo::OPERATION op, const char* tip) {
		const bool active = (gizmoOp_ == op);
		if (active)
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		if (ImGui::SmallButton(label))
			gizmoOp_ = op;
		if (active)
			ImGui::PopStyleColor();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", tip);
	};

	opButton("Move", ImGuizmo::TRANSLATE, "Translate (W)");
	opButton("Rotate", ImGuizmo::ROTATE, "Rotate (E)");
	opButton("Scale", ImGuizmo::SCALE, "Scale (R)");

	ImGui::Separator();
	if (ImGui::SmallButton(gizmoMode_ == ImGuizmo::WORLD ? "World" : "Local"))
		gizmoMode_ = (gizmoMode_ == ImGuizmo::WORLD) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Gizmo space (scale is always local)");

	ImGui::Separator();
	ImGui::Checkbox("Snap", &gizmoSnap_);
	if (gizmoSnap_)
	{
		ImGui::SetNextItemWidth(70);
		switch (gizmoOp_)
		{
		case ImGuizmo::ROTATE: ImGui::DragFloat("##snap", &snapRotateDeg_, 0.5f, 1.0f, 90.0f, "%.0f deg"); break;
		case ImGuizmo::SCALE:  ImGui::DragFloat("##snap", &snapScale_, 0.01f, 0.01f, 10.0f, "%.2f"); break;
		default:               ImGui::DragFloat("##snap", &snapTranslate_, 0.05f, 0.01f, 100.0f, "%.2f"); break;
		}
	}

	if (!lastSaveMessage_.empty())
	{
		ImGui::Separator();
		ImGui::TextDisabled("%s", lastSaveMessage_.c_str());
	}
}

// -------------------------------------------------------------------- viewport ---

void st::EditorUI::DrawEditorViewportToolbar()
{
	if (!ImGui::BeginMenuBar())
		return;

	ImGui::Checkbox("Show Game Preview", &showGamePreview_);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Render this viewport through the game's own renderer - the live "
			"graphics settings (AO, bloom, tonemap, exposure, colour grading), the game "
			"path's colorspace and layer mask, and its custom post process passes "
			"(projectors, lasers). Off falls back to bare RenderPath3D defaults.\n\n"
			"Debug draws are separate and stay on either way - see Debug in the menu bar "
			"above.");

	ImGui::Separator();
	ImGui::TextDisabled("%u x %u", editorViewSize_.x, editorViewSize_.y);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("The editor viewport always follows its panel exactly, so picking "
			"and the gizmo map 1:1.");

	ImGui::EndMenuBar();
}

void st::EditorUI::DrawGameViewportToolbar()
{
	if (!ImGui::BeginMenuBar())
		return;

	// Free Aspect is the only mode a shipping build has: the path follows the window canvas.
	//	Anything else forces st::ProjectorRenderPath::forcedResolution and the image below is
	//	letterboxed into whatever shape the dock happens to give the panel.
	if (ImGui::BeginMenu(gameRes_.fixed ? "Fixed Resolution" : "Free Aspect"))
	{
		if (ImGui::MenuItem("Free Aspect", nullptr, !gameRes_.fixed))
			gameRes_.fixed = false;
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Render at the window canvas size, the way the game ships.");
		if (ImGui::MenuItem("Fixed Resolution", nullptr, gameRes_.fixed))
			gameRes_.fixed = true;
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Render the GAME at a fixed size regardless of the window - the "
				"honest way to check a 4K frame, a phone crop or a 2.39:1 letterbox. Cleared "
				"automatically when editor mode leaves.");
		ImGui::EndMenu();
	}

	ImGui::Separator();
	ImGui::BeginDisabled(!gameRes_.fixed);
	DrawResolutionMenus(gameRes_.width, gameRes_.height, gameRes_.ratio);
	ImGui::EndDisabled();

	// What the path is ACTUALLY at, not what the menu asked for: a forced size only lands on
	//	the next frame's PreUpdate, and while Free Aspect is on the window is what decides.
	ImGui::Separator();
	if (gamePath_ != nullptr)
	{
		const uint32_t w = gamePath_->GetPhysicalWidth();
		const uint32_t h = gamePath_->GetPhysicalHeight();
		ImGui::TextDisabled("%u x %u  %.3f", w, h, (float)w / (float)std::max(1u, h));
	}
	else
	{
		ImGui::TextDisabled("%d x %d", gameRes_.width, gameRes_.height);
	}

	// Right-aligned reminder of what this window is, matching a Camera View's "view only".
	{
		const char* note = "click to play";
		const float w = ImGui::CalcTextSize(note).x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
		if (ImGui::GetCursorPosX() < ImGui::GetWindowWidth() - w)
			ImGui::SetCursorPosX(ImGui::GetWindowWidth() - w);
		ImGui::TextDisabled("%s", note);
	}

	ImGui::EndMenuBar();
}

void st::EditorUI::DrawViewport(const char* title, bool* p_open, wi::RenderPath3D& path,
	CameraComponent& cam, bool exactFit, wi::scene::Scene& scene, Entity& selected)
{
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	const bool open = ImGui::Begin(title, p_open, ImGuiWindowFlags_MenuBar);
	ImGui::PopStyleVar();
	if (!open)
	{
		ImGui::End();
		return;
	}

	// exactFit is what tells the two viewports apart everywhere in this function: true is
	//	the Editor Viewport, false is the Game Viewport. Their menu bars differ for the same
	//	reason the rest does - one previews, the other plays.
	if (exactFit)
		DrawEditorViewportToolbar();
	else
		DrawGameViewportToolbar();

	// AFTER the menu bar: it takes a row off the top, and the editor path is sized from this.
	const ImVec2 avail  = ImGui::GetContentRegionAvail();
	const ImVec2 cursor = ImGui::GetCursorScreenPos();
	const bool   usable = (avail.x >= 16.0f && avail.y >= 16.0f);

	if (exactFit)
	{
		// The editor path follows its panel exactly, so the gizmo and picking work in
		// unscaled pixels. RenderPath2D::Update picks the new size up and reallocates.
		editorViewSize_ = XMUINT2((uint32_t)std::max(16.0f, avail.x), (uint32_t)std::max(16.0f, avail.y));
		editorViewDPI_  = ImGui::GetMainViewport()->DpiScale * 96.0f;
		if (editorViewDPI_ <= 0.0f)
			editorViewDPI_ = 96.0f;
		editorViewLive_ = usable;
	}

	const wi::graphics::Texture* tex = path.GetLastPostprocessRT();
	ImVec2 imagePos  = cursor;
	ImVec2 imageSize = avail;
	bool   imageDrawn = false;

	if (usable && tex != nullptr && tex->IsValid())
	{
		if (!exactFit)
		{
			// The game path stays at the window canvas size: letterbox rather than
			// stretch, so the game's aspect ratio does not change with the dock layout.
			const wi::graphics::TextureDesc& d = tex->GetDesc();
			const float texAspect   = (float)d.width / (float)std::max(1u, d.height);
			const float panelAspect = avail.x / avail.y;
			if (panelAspect > texAspect)
				imageSize = ImVec2(avail.y * texAspect, avail.y);
			else
				imageSize = ImVec2(avail.x, avail.x / texAspect);
			imagePos = ImVec2(cursor.x + (avail.x - imageSize.x) * 0.5f,
				cursor.y + (avail.y - imageSize.y) * 0.5f);

			ImGui::GetWindowDrawList()->AddRectFilled(cursor,
				ImVec2(cursor.x + avail.x, cursor.y + avail.y), IM_COL32(0, 0, 0, 255));
		}

		ImGui::SetCursorScreenPos(imagePos);
		ImGui::Image((ImTextureID)(uintptr_t)tex, imageSize);
		imageDrawn = true;

		// Drop a MODEL out of the Resource Explorer onto the viewport and it is merged into
		//	the scene at the spawn point, the same place the Create menu puts a new object.
		//	Anything that is not a model declines here, so dragging a texture over the image
		//	shows no drop cue and falls through to whatever else wants it.
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(st::SIMTARY_ASSET_PAYLOAD))
			{
				if (p->DataSize == (int)sizeof(st::AssetPayload))
				{
					const st::AssetPayload* a = (const st::AssetPayload*)p->Data;
					if (a->IsModel())
						QueueImportModel(a->path);   // loaded next frame, on the main thread
					else
						wi::backlog::post(std::string("Editor: ") + a->path +
							" is not a model; drop it on an asset field in Properties instead.",
							wi::backlog::LogLevel::Warning);
				}
			}
			ImGui::EndDragDropTarget();
		}
	}
	else
	{
		ImGui::Dummy(avail);
		ImGui::SetCursorScreenPos(ImVec2(cursor.x + 8, cursor.y + 8));
		ImGui::TextDisabled("waiting for the first frame...");
	}

	// Hover is measured against the image RECT, not an ImGui item. There deliberately is no
	//	invisible button over the viewport: ImGuizmo::CanActivate() refuses to begin a drag
	//	while ImGui reports any item hovered or active, so a button covering the image stops
	//	the gizmo from ever engaging. Ownership of the mouse during a drag is handled after
	//	Manipulate instead — see the ActiveId claim in DrawGizmo.
	const bool hovered = imageDrawn
		&& ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)
		&& ImGui::IsMouseHoveringRect(imagePos,
			ImVec2(imagePos.x + imageSize.x, imagePos.y + imageSize.y));
	const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

	// The Game Viewport is the only panel that hands input back to the game. Click into it
	//	to play; click anywhere else in the editor and the game stops hearing the keyboard.
	//	Keyboard follows focus, mouse follows the pointer — so clicking the Hierarchy while
	//	the game still has focus does not also click in the game.
	if (!exactFit)
	{
		if (focused) gameViewFocused_ = true;
		if (hovered) gameViewHovered_ = true;
	}

	// From here down: editing. The Game Viewport is play-only — no freecam, no gizmo, no
	//	click-to-select. Its clicks belong to the game, and a gizmo living in the same panel
	//	would be fighting the player for every one of them. Edit in the Editor Viewport.
	if (!exactFit)
	{
		// Say which way input is pointing. Without this "the game ignores my keys" is a
		//	mystery — the panel looks identical whether it has input or not.
		if (imageDrawn && !focused && !st::InputSystem::Get().IsMouseCaptured())
		{
			ImGui::GetWindowDrawList()->AddText(ImVec2(imagePos.x + 10, imagePos.y + 8),
				IM_COL32(255, 200, 90, 220), "click to give the game input");
		}
		ImGui::End();
		return;
	}

	// Freecam: only the editor viewport has one, and only while the panel has the mouse.
	if (exactFit)
	{
		UpdateFreeCam(ImGui::GetIO().DeltaTime, hovered || (focused && ImGui::IsMouseDown(ImGuiMouseButton_Right)));

		// F frames the selection. Selecting from the Hierarchy can easily pick something
		// behind the camera, where a perfectly working gizmo is simply off screen.
		if (hovered && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false))
			FrameSelected(scene, selected);
	}

	// The gizmo is drawn in EVERY visible viewport, not just the focused one — selecting an
	//	entity from the Hierarchy puts focus on that panel, and a gizmo that disappears the
	//	moment you pick something is useless. Only one viewport can actually drag it: each
	//	passes its own ImGuizmo id, and ImGuizmo itself refuses input unless its drawlist's
	//	window is the one under the pointer.
	bool gizmoBusy = false;
	if (imageDrawn && selected != INVALID_ENTITY)
		gizmoBusy = DrawGizmo(scene, selected, cam, imagePos, imageSize);

	// Click to select. Skipped while the pointer is on the gizmo, otherwise grabbing an
	// axis would immediately reselect whatever is behind it.
	if (hovered && !gizmoBusy && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		const ImVec2 m = ImGui::GetIO().MousePos;
		const float u = (m.x - imagePos.x) / std::max(1.0f, imageSize.x);
		const float v = (m.y - imagePos.y) / std::max(1.0f, imageSize.y);
		if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f)
		{
			const long cx = (long)(u * path.GetLogicalWidth());
			const long cy = (long)(v * path.GetLogicalHeight());
			const wi::primitive::Ray ray = wi::renderer::GetPickRay(cx, cy, path, cam);
			const wi::scene::PickResult hit = wi::scene::Pick(ray,
				wi::enums::FILTER_OPAQUE | wi::enums::FILTER_TRANSPARENT, ~0u, scene);
			selected = hit.entity; // a miss returns INVALID_ENTITY, which deselects
		}
	}

	ImGui::End();
}

bool st::EditorUI::DrawGizmo(wi::scene::Scene& scene, Entity selected, const CameraComponent& cam,
	ImVec2 imagePos, ImVec2 imageSize)
{
	TransformComponent* t = scene.transforms.GetComponent(selected);
	if (t == nullptr)
		return false;

	ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
	ImGuizmo::SetRect(imagePos.x, imagePos.y, imageSize.x, imageSize.y);
	ImGuizmo::SetOrthographic(cam.IsOrtho());
	// Per-window id, so the two viewports keep separate drag state.
	ImGuizmo::SetID((int)ImGui::GetCurrentWindow()->ID);

	// Keep a pre-drag copy while the gizmo is idle: once ImGuizmo reports a drag it has
	//	already written the first delta, which is too late to capture "before".
	if (!gizmoDragging_)
		gizmoPreDrag_ = TransformSnapshot::Capture(*t);

	// TransformComponent::world is already expressed in the same origin-relative float
	// space as the camera's view matrix, so the gizmo needs no rebasing on the way in.
	XMFLOAT4X4 world = t->world;
	XMFLOAT4X4 view = cam.View;
	XMFLOAT4X4 proj = cam.Projection;

	float snapValue[3] = { snapTranslate_, snapTranslate_, snapTranslate_ };
	if (gizmoOp_ == ImGuizmo::ROTATE) snapValue[0] = snapValue[1] = snapValue[2] = snapRotateDeg_;
	if (gizmoOp_ == ImGuizmo::SCALE)  snapValue[0] = snapValue[1] = snapValue[2] = snapScale_;

	const bool changed = ImGuizmo::Manipulate(
		&view._11, &proj._11, gizmoOp_, gizmoMode_, &world._11,
		nullptr, gizmoSnap_ ? snapValue : nullptr);

	// Read the hover/drag state while this viewport's id is still current — IsUsing()
	// compares against it, so resetting the id first would always answer false.
	const bool busy = ImGuizmo::IsOver() || ImGuizmo::IsUsing();

	if (ImGuizmo::IsUsing())
	{
		if (!gizmoDragging_)
		{
			gizmoDragging_   = true;
			gizmoDragEntity_ = selected; // Draw() closes the undo step when the drag ends
		}

		// Park ImGui's ActiveId on a private id for the duration of the drag, so no item in
		//	any other panel can be hovered or clicked while the pointer sweeps across it.
		//	ImGuizmo draws straight into a drawlist and owns no ImGui item, which is why a
		//	drag over the Hierarchy used to light its rows up and could drop the mouse-up on
		//	one. Claimed only AFTER Manipulate: CanActivate() refuses to start a drag while
		//	any item is active, so claiming earlier would stop the gizmo engaging at all.
		ImGuiContext& g = *ImGui::GetCurrentContext();
		const ImGuiID dragID = ImGui::GetID("##simtary_gizmo_drag");
		if (g.ActiveId != dragID)
			ImGui::SetActiveID(dragID, ImGui::GetCurrentWindow());
		else
			ImGui::KeepAliveID(dragID);
	}

	if (!changed)
		return busy;

	// The gizmo edited a WORLD matrix; the component stores a LOCAL one. Divide the
	// parent out first, or dragging a child of a rig would apply the parent transform twice.
	XMMATRIX localMatrix = XMLoadFloat4x4(&world);
	bool parented = false;
	if (const HierarchyComponent* h = scene.hierarchy.GetComponent(selected))
	{
		if (const TransformComponent* parent = scene.transforms.GetComponent(h->parentID))
		{
			localMatrix = XMMatrixMultiply(localMatrix,
				XMMatrixInverse(nullptr, parent->GetWorldMatrix()));
			parented = true;
		}
	}

	// Write back only what this operation actually changed. Decomposing and recomposing the
	//	whole transform every drag frame slowly grinds a small scale (0.05 on an imported prop,
	//	say) into noise, and a translate has no business rewriting rotation at all.
	if (gizmoOp_ == ImGuizmo::TRANSLATE)
	{
		XMStoreFloat3(&t->translation_local, localMatrix.r[3]);
	}
	else
	{
		XMVECTOR s, r, tr;
		if (!XMMatrixDecompose(&s, &r, &tr, localMatrix))
		{
			// A zero or sheared scale somewhere in the parent chain. Doing nothing quietly is
			// exactly what "this object is locked" looks like from the outside, so say it once.
			static Entity warned = INVALID_ENTITY;
			if (warned != selected)
			{
				warned = selected;
				wi::backlog::post("Editor: entity " + std::to_string(selected) +
					" has a transform that cannot be decomposed (zero or sheared scale in its"
					" parent chain); rotate/scale is unavailable on it.",
					wi::backlog::LogLevel::Warning);
			}
			return busy;
		}
		if (gizmoOp_ == ImGuizmo::SCALE)
			XMStoreFloat3(&t->scale_local, s);
		else
			XMStoreFloat4(&t->rotation_local, r);
	}

	// Push the local offset back into the 64-bit absolute position — ALWAYS, parented or not.
	//
	//	For a LARGE_WORLD transform, translation_local is a DERIVED value, not the truth:
	//	Scene::RunTransformUpdateSystem calls UpdateTransform() on every transform (children
	//	included), and that rebases translation_local from world_translation_* before
	//	RunHierarchyUpdateSystem folds in the parent chain. A gizmo drag that only wrote the
	//	local offset was therefore erased before the next frame was drawn — the object looked
	//	locked, and only the World pos (abs) field could move it.
	//
	//	And TransformComponent::Serialize sets the flag on EVERY transform it reads (its read
	//	path calls SetWorldPosition unconditionally), so effectively everything loaded from a
	//	.wiscene is a large-world transform, however deep in a hierarchy it sits.
	//
	//	For a transform without the flag this is just bookkeeping, and wanted bookkeeping: the
	//	absolute position is the field that gets serialized, so keeping it current is what
	//	stops the object reappearing at the world origin on the next load.
	t->SyncWorldFromLocal(wi::scene::GetRenderOrigin());

	t->SetDirty();
	return busy;
}

// -------------------------------------------------------------------- free cam ---

void st::EditorUI::FrameSelected(wi::scene::Scene& scene, Entity selected)
{
	const TransformComponent* t = scene.transforms.GetComponent(selected);
	if (t == nullptr)
		return;

	// Pull back along the current view direction by the object's own size, so the framing
	// keeps whatever angle the user was already looking from.
	float radius = 2.0f;
	if (scene.objects.Contains(selected))
	{
		const size_t index = scene.objects.GetIndex(selected);
		if (index < scene.aabb_objects.size())
			radius = std::max(0.25f, scene.aabb_objects[index].getRadius());
	}

	const XMVECTOR q       = XMQuaternionRotationRollPitchYaw(camRot_.x, camRot_.y, camRot_.z);
	const XMVECTOR forward = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), q);
	const XMFLOAT3 target  = t->GetPosition();
	XMStoreFloat3(&camPos_, XMVectorSubtract(XMLoadFloat3(&target),
		XMVectorScale(forward, radius * 3.0f)));
}

void st::EditorUI::UpdateFreeCam(float dt, bool interactive)
{
	editorCamActive_ = interactive;
	if (!interactive || dt <= 0.0f)
		return;

	ImGuiIO& io = ImGui::GetIO();

	// Right mouse held = "flying": look with the mouse, WASD/QE to move. Releasing it
	// hands W/E/R back to the gizmo-operation shortcuts, the way an editor expects.
	const bool flying = ImGui::IsMouseDown(ImGuiMouseButton_Right);

	// Ask for the cursor. Draw() reconciles the request at the end of the frame, so the
	// look is released even when the panel stops being drawn mid-drag.
	freeCamLookWanted_ = flying;

	if (flying)
	{
		// SDL relative-mouse mode (via InputSystem): the cursor is hidden and the pointer
		// cannot leave the window, so a long drag never stalls against a screen edge. The
		// delta has to come from InputSystem too — in relative mode SDL stops moving the
		// pointer, so ImGui's own io.MouseDelta is zero.
		const XMFLOAT2 d = st::InputSystem::Get().MouseDelta();
		camRot_.y += d.x * camSens_;
		camRot_.x += d.y * camSens_;
		camRot_.x = std::clamp(camRot_.x, -kPitchLimit, kPitchLimit);
		ImGui::SetMouseCursor(ImGuiMouseCursor_None);
	}

	float fwd = 0.0f;
	float strafe = 0.0f;
	float up = 0.0f;

	// Wheel sets the fly speed while flying, and dollies the camera otherwise.
	if (io.MouseWheel != 0.0f)
	{
		if (flying)
			camSpeed_ = std::clamp(camSpeed_ * (io.MouseWheel > 0.0f ? 1.25f : 0.8f), 0.05f, 2000.0f);
		else
			fwd += io.MouseWheel * 25.0f;
	}

	if (flying && !io.WantTextInput)
	{
		if (ImGui::IsKeyDown(ImGuiKey_W)) fwd    += 1.0f;
		if (ImGui::IsKeyDown(ImGuiKey_S)) fwd    -= 1.0f;
		if (ImGui::IsKeyDown(ImGuiKey_D)) strafe += 1.0f;
		if (ImGui::IsKeyDown(ImGuiKey_A)) strafe -= 1.0f;
		if (ImGui::IsKeyDown(ImGuiKey_E)) up     += 1.0f;
		if (ImGui::IsKeyDown(ImGuiKey_Q)) up     -= 1.0f;
	}

	if (fwd == 0.0f && strafe == 0.0f && up == 0.0f)
		return;

	const bool boost = ImGui::IsKeyDown(ImGuiKey_LeftShift);
	const float step = camSpeed_ * (boost ? camBoost_ : 1.0f) * dt;

	const XMVECTOR q       = XMQuaternionRotationRollPitchYaw(camRot_.x, camRot_.y, camRot_.z);
	const XMVECTOR forward = XMVector3Rotate(XMVectorSet(0, 0, 1, 0), q);
	const XMVECTOR right   = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), q);
	const XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);

	XMVECTOR pos = XMLoadFloat3(&camPos_);
	pos = XMVectorAdd(pos, XMVectorScale(forward, fwd * step));
	pos = XMVectorAdd(pos, XMVectorScale(right, strafe * step));
	pos = XMVectorAdd(pos, XMVectorScale(worldUp, up * step));
	XMStoreFloat3(&camPos_, pos);
}

// ------------------------------------------------------------------ scene save ---

void st::EditorUI::SaveScene(wi::scene::Scene& scene, const std::string& path)
{
	if (path.empty())
	{
		RequestSaveAs();
		return;
	}

	// .stsd is the native format and the default; .wiscene is still written on request,
	// because it is what the standalone Wicked editor opens and what the converter reads.
	std::string ext = wi::helper::GetExtensionFromFileName(path);
	for (char& c : ext)
		if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');

	const bool ok = (ext == "wiscene") ? SaveSceneArchive(scene, path)
	                                   : SaveSceneDescriptor(scene, path);
	if (ok)
	{
		scenePath_ = path;
		wi::backlog::post("Editor: saved scene to " + path);
	}
	else
	{
		wi::backlog::post("Editor: FAILED to save scene to " + path, wi::backlog::LogLevel::Error);
	}
}

bool st::EditorUI::SaveSceneArchive(wi::scene::Scene& scene, const std::string& path)
{
	wi::Archive archive; // default-constructed = empty, write mode
	scene.Serialize(archive);
	if (!archive.SaveFile(path))
	{
		lastSaveMessage_ = "SAVE FAILED";
		return false;
	}
	lastSaveMessage_ = "saved " + wi::helper::GetFileNameFromPath(path) + " (.wiscene)";
	return true;
}

bool st::EditorUI::SaveSceneDescriptor(wi::scene::Scene& scene, const std::string& path)
{
	// Embedding is turned ON for exactly this serialize, then put back.
	//
	// It is not for the bytes — the .stsd deliberately carries none. It is for the LIST:
	// the resource block is the only place the engine records which files a scene
	// actually uses, and without it the .stsd has no reference list, so nothing can
	// report "13 of 14 assets" during a load or answer MissingAssetsFor() before the
	// world comes up untextured. SplitWiscene then lifts the block straight back out,
	// which is the same code path the build-time packer uses and is proven byte-exact.
	const wi::resourcemanager::Mode previousMode = wi::resourcemanager::GetMode();
	wi::resourcemanager::SetMode(wi::resourcemanager::Mode::EMBED_FILE_DATA);
	wi::Archive archive;
	scene.Serialize(archive);
	wi::resourcemanager::SetMode(previousMode);

	wi::vector<uint8_t> bytes;
	archive.WriteData(bytes);

	const std::string stem = wi::helper::GetFileNameFromPath(path);
	const std::string name = wi::helper::RemoveExtension(stem);

	std::string error;
	st::asset::SceneDescriptor descriptor;
	st::asset::WisceneSplit    split;
	if (!st::asset::BuildSceneDescriptorFromMemory(bytes.data(), bytes.size(), name, stem,
	                                               std::string(), descriptor, split, &error))
	{
		lastSaveMessage_ = "SAVE FAILED: " + error;
		return false;
	}

	// Anything the mounted packages already hold needs no copy — the map just references
	// it. Whatever is left is content this session introduced that lives nowhere the next
	// run would find it, so it goes into a package beside the map and is mounted at once.
	// Without this the map saves "successfully" and reloads untextured.
	std::vector<size_t> unpacked;
	for (size_t i = 0; i < split.resources.size(); ++i)
	{
		if (!st::AssetSystem::Get().Exists(descriptor.assets[i].path))
			unpacked.push_back(i);
	}

	const std::string dir = wi::helper::GetDirectoryFromPath(path);
	if (!unpacked.empty())
	{
		st::asset::AssetPackWriter writer;
		st::asset::PackOptions options;
		if (!writer.Begin(dir.empty() ? "." : dir, name, options, &error))
		{
			lastSaveMessage_ = "SAVE FAILED: " + error;
			return false;
		}
		for (size_t i : unpacked)
		{
			const st::asset::EmbeddedResource& r = split.resources[i];
			if (!writer.AddAuto(descriptor.assets[i].path, split.Bytes() + r.offset, r.size,
			                    st::asset::AssetFlag_FromScene, &error))
			{
				lastSaveMessage_ = "SAVE FAILED: " + error;
				return false;
			}
		}
		if (!writer.Finish(&error))
		{
			lastSaveMessage_ = "SAVE FAILED: " + error;
			return false;
		}
		descriptor.packUuidLo = writer.UuidLo();
		descriptor.packUuidHi = writer.UuidHi();

		const std::string index = (dir.empty() ? std::string() : dir) + name + ".strd";
		if (st::AssetSystem::Get().Mount(index, "assets/", &error))
			st::AssetSystem::Get().Install();
	}

	if (!st::asset::WriteSceneDescriptor(path, descriptor, st::asset::SceneWriteOptions{}, &error))
	{
		lastSaveMessage_ = "SAVE FAILED: " + error;
		return false;
	}

	lastSaveMessage_ = "saved " + stem + "  (" + std::to_string(descriptor.assets.size()) +
	                   " assets" + (unpacked.empty() ? ", all already packed"
	                                                 : ", " + std::to_string(unpacked.size()) +
	                                                   " written to " + name + ".strd") + ")";
	return true;
}

// ─── import into the scene ─────────────────────────────────────────────────────
// Distinct from the Resource Explorer's import, which adds a file to the asset PACKAGE.
// This one merges a model into the live scene and puts it where the editor camera is
// looking, which is the other thing "import" reasonably means in an editor.

bool st::EditorUI::IsSceneImportPath(const std::string& path)
{
	const std::string ext = wi::helper::toLower(wi::helper::GetExtensionFromFileName(path));
	// The engine's own two formats, plus whatever the model importers were built with. The
	//	second half is asked rather than listed, so adding a backend does not mean editing
	//	this test, the file dialog and the drop handler as well.
	if (ext == "wiscene" || ext == "stsd") return true;
	return st::model::CanImport(path);
}

void st::EditorUI::QueueImportModel(const std::string& path)
{
	if (path.empty())
		return;
	std::lock_guard<std::mutex> lock(pendingImportMutex_);
	pendingImportPaths_.push_back(path);
}

void st::EditorUI::RequestImportModel()
{
	if (importDialogOpen_)
		return;
	importDialogOpen_ = true;

	wi::helper::FileDialogParams params;
	params.type = wi::helper::FileDialogParams::OPEN;
	params.description = ImportFilterDescription();
	params.extensions.push_back("stsd");
	params.extensions.push_back("wiscene");
	size_t modelExtCount = 0;
	const char* const* modelExts = st::model::SupportedExtensions(modelExtCount);
	for (size_t i = 0; i < modelExtCount; ++i)
		params.extensions.push_back(modelExts[i]);

	// Runs on the dialog's own thread: park the path, let Draw() do the load.
	wi::helper::FileDialog(params,
		[this](std::string fileName) {
			QueueImportModel(fileName);
			importDialogOpen_ = false;
		},
		[this]() {
			importDialogOpen_ = false;
		});
}

void st::EditorUI::FlushPendingImport(wi::scene::Scene& scene)
{
	// One at a time while the options panel is up: it is asking about a specific file, and
	//	loading the rest behind it would answer for them.
	if (!importOptionsPath_.empty())
		return;

	std::string path;
	{
		std::lock_guard<std::mutex> lock(pendingImportMutex_);
		if (pendingImportPaths_.empty())
			return;
		path = pendingImportPaths_.front();
		pendingImportPaths_.erase(pendingImportPaths_.begin());
	}

	if (importAskEveryTime_)
	{
		// Hand it to the panel; the import happens when the user presses Import there.
		importOptionsPath_      = path;
		importOptionsRequested_ = true;
		return;
	}

	const Entity root = ImportModelAtSpawn(scene, path);
	if (root != INVALID_ENTITY)
		pendingSelection_ = root;
}

wi::ecs::Entity st::EditorUI::ImportModelAtSpawn(wi::scene::Scene& scene, const std::string& path)
{
	// A load creates the root PLUS every mesh, material, armature and animation entity behind
	//	it, and most of those are not in the hierarchy under the root. Undo has to own all of
	//	them or it would delete the root and leave the contents behind, so the set is worked
	//	out by diffing the scene's entity list around the load. That is one pass over the
	//	component managers on either side, which is nothing next to the load itself.
	auto gather = [](wi::scene::Scene& s, std::unordered_set<Entity>& out) {
		for (auto& kv : s.componentLibrary.entries)
		{
			const auto* mgr = kv.second.component_manager.get();
			if (!mgr) continue;
			for (Entity e : mgr->GetEntityArray())
				out.insert(e);
		}
	};

	std::unordered_set<Entity> before;
	gather(scene, before);

	const std::string ext = wi::helper::toLower(wi::helper::GetExtensionFromFileName(path));

	Entity root = INVALID_ENTITY;
	std::string error;
	std::string detail;

	// Loaded at the ORIGIN and moved afterwards rather than loaded through a translation
	//	matrix: PlaceEntityAt is the one place that also writes the 64-bit absolute position,
	//	and a model placed only in local space returns to the world origin on the next save.
	//
	//	Three loaders, by extension. .stsd is the native descriptor, .wiscene is a wi::Archive
	//	the engine reads directly, and everything else is an interchange format that
	//	Framework/io/model converts into components.
	if (ext == "stsd")
	{
		root = st::AssetSystem::Get().LoadScene(scene, path, XMMatrixIdentity(), true, nullptr, &error);
	}
	else if (ext == "wiscene")
	{
		root = wi::scene::LoadModel(scene, path, XMMatrixIdentity(), true);
		if (root == INVALID_ENTITY)
			error = "could not load " + path;
	}
	else
	{
		const st::model::ImportResult imported = st::model::Import(scene, path, importOptions_);
		root  = imported.root;
		error = imported.error;
		if (imported.ok())
		{
			char summary[192];
			std::snprintf(summary, sizeof(summary),
				"  (%u meshes, %u materials, %u textures, %u bones, %u animations)",
				imported.meshes, imported.materials, imported.textures,
				imported.bones, imported.animations);
			detail = summary;
		}
	}

	if (root == INVALID_ENTITY)
	{
		lastImportMessage_ = "IMPORT FAILED: " + (error.empty() ? path : error);
		wi::backlog::post("Editor: " + lastImportMessage_, wi::backlog::LogLevel::Warning);
		return INVALID_ENTITY;
	}

	// In front of the free camera, same spot the Create menu uses — or at the world origin
	//	when the model was authored around one.
	PlaceEntityAt(scene, root, importPlaceAtCamera_ ? SpawnPoint() : XMFLOAT3(0, 0, 0),
		INVALID_ENTITY);

	// Name the root after the file, so the Hierarchy shows something better than "Entity 41".
	const std::string stem = wi::helper::GetFileNameFromPath(path);
	if (!stem.empty())
	{
		if (wi::scene::NameComponent* n = scene.names.GetComponent(root))
			n->name = stem;
		else
			scene.names.Create(root).name = stem;
	}

	wi::vector<Entity> created;
	std::unordered_set<Entity> after;
	gather(scene, after);
	for (Entity e : after)
		if (before.count(e) == 0)
			created.push_back(e);

	// Put the whole import under the one root, not just the parts that happen to carry a
	//	transform. A load creates mesh, material and animation-data entities that have no
	//	TransformComponent and therefore no parent, and the Hierarchy renders anything without
	//	a parent as a top-level row — which is how importing one character adds two hundred
	//	loose rows to the tree. Component_Attach only rebases a transform when BOTH sides have
	//	one, so attaching a material is exactly the metadata edit it looks like.
	if (importGroupUnderRoot_)
	{
		for (Entity e : created)
		{
			if (e == root) continue;
			if (scene.hierarchy.Contains(e)) continue;   // the model's own nodes already are
			scene.Component_Attach(e, root, true);
		}
	}
	// The root first, so an undo that walks the list tears the hierarchy down from the top.
	for (size_t i = 1; i < created.size(); ++i)
	{
		if (created[i] == root) { std::swap(created[0], created[i]); break; }
	}
	history_.RecordCreatedMany(scene, created, "Import Model");

	lastImportMessage_ = "imported " + stem + "  (" + std::to_string(created.size()) +
		" entities)" + detail;
	wi::backlog::post("Editor: " + lastImportMessage_ +
		(importPlaceAtCamera_ ? " in front of the camera" : " at the world origin"));
	return root;
}

std::string st::EditorUI::ImportFilterDescription()
{
	// The Win32 dialog shows this string VERBATIM as the filter label — it does not derive
	//	one from the extension list, so a bare "Model or scene" tells the user nothing about
	//	what will actually open. Spell the extensions into the label, and build the list from
	//	the loaders rather than hard-coding it, so it cannot drift from what is compiled in.
	std::string description = "Model or scene (*.stsd;*.wiscene";
	size_t modelExtCount = 0;
	const char* const* modelExts = st::model::SupportedExtensions(modelExtCount);
	for (size_t i = 0; i < modelExtCount; ++i)
	{
		description += ";*.";
		description += modelExts[i];
	}
	description += ")";
	return description;
}

void st::EditorUI::DrawImportOptions(wi::scene::Scene& scene)
{
	// OpenPopup has to be called from inside the frame, not from the flush that decided a
	//	panel was needed, so the request is raised there and acted on here.
	if (importOptionsRequested_)
	{
		ImGui::OpenPopup(kImportOptions);
		importOptionsRequested_ = false;
	}

	// Centred over the viewport: this is a decision the user has to make before anything
	//	happens, and a modal parked in a corner reads as a panel that can be ignored.
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(vp->GetCenter().x, vp->GetCenter().y), ImGuiCond_Appearing,
		ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(430, 0), ImGuiCond_Appearing);

	if (!ImGui::BeginPopupModal(kImportOptions, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		return;

	const std::string file = wi::helper::GetFileNameFromPath(importOptionsPath_);
	const std::string ext  = wi::helper::toLower(
		wi::helper::GetExtensionFromFileName(importOptionsPath_));
	ImGui::TextUnformatted(file.c_str());
	ImGui::TextDisabled("%s", importOptionsPath_.c_str());
	ImGui::Separator();

	// The two options that decide where things END UP, first, because they are the two a
	//	user changes per import rather than once.
	ImGui::Checkbox("Import into a Hierarchy group", &importGroupUnderRoot_);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip(
			"Everything the import creates goes under ONE node named after the file.\n\n"
			"Without this only the model's own nodes are parented: its meshes, materials\n"
			"and animation curves carry no transform, so they land at the top of the\n"
			"Hierarchy as hundreds of loose rows. Grouping also makes the whole import one\n"
			"thing to select, move, hide or delete.");

	ImGui::Checkbox("Place in front of the camera", &importPlaceAtCamera_);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Off puts it at the world origin, which is what you want when the\n"
			"model was authored around its own origin and you are placing it by hand.");

	const bool isModel = st::model::CanImport(importOptionsPath_);
	ImGui::Separator();

	if (!isModel)
	{
		ImGui::TextDisabled("%s is loaded by the engine directly; the options below apply\n"
			"to imported model formats only.", ext.c_str());
	}

	ImGui::BeginDisabled(!isModel);
	ImGui::Checkbox("Animations", &importOptions_.importAnimations);
	ImGui::SameLine();
	ImGui::Checkbox("Skins", &importOptions_.importSkins);
	ImGui::SameLine();
	ImGui::Checkbox("Lights", &importOptions_.importLights);
	ImGui::SameLine();
	ImGui::Checkbox("Cameras", &importOptions_.importCameras);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Lights and cameras are off by default: a prop rarely wants the\n"
			"exporter's lighting rig, and finding them again afterwards is a chore.");

	ImGui::Checkbox("Generate missing normals", &importOptions_.generateMissingNormals);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("A mesh with no normals renders black. OBJ ships without them often.");

	ImGui::SetNextItemWidth(120);
	ImGui::DragFloat("Scale", &importOptions_.scale, 0.01f, 0.0001f, 1000.0f, "%.4f");
	ImGui::SameLine();
	// Presets rather than arithmetic: the two cases that come up are a model authored in
	//	centimetres and one authored in inches, and neither is worth typing out.
	if (ImGui::SmallButton("1")) importOptions_.scale = 1.0f;
	ImGui::SameLine();
	if (ImGui::SmallButton("0.01 (cm)")) importOptions_.scale = 0.01f;
	ImGui::SameLine();
	if (ImGui::SmallButton("0.0254 (in)")) importOptions_.scale = 0.0254f;
	ImGui::EndDisabled();

	ImGui::Separator();
	ImGui::Checkbox("Ask every time", &importAskEveryTime_);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Off imports straight away with these settings. Turn it back on from\n"
			"Scene > Import options...");

	ImGui::Separator();
	if (ImGui::Button("Import", ImVec2(120, 0)))
	{
		const std::string path = importOptionsPath_;
		importOptionsPath_.clear();
		ImGui::CloseCurrentPopup();
		const Entity root = ImportModelAtSpawn(scene, path);
		if (root != INVALID_ENTITY)
			pendingSelection_ = root;
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel", ImVec2(120, 0)))
	{
		// Drop the rest of the queue too: cancelling one file out of a multi-file drop and
		//	then being asked about the next four is not what Cancel means.
		{
			std::lock_guard<std::mutex> lock(pendingImportMutex_);
			pendingImportPaths_.clear();
		}
		importOptionsPath_.clear();
		lastImportMessage_ = "import cancelled";
		ImGui::CloseCurrentPopup();
	}

	ImGui::EndPopup();
}

void st::EditorUI::RequestSaveAs(const char* defaultExt)
{
	if (saveDialogOpen_)
		return;
	saveDialogOpen_ = true;
	saveDefaultExt_ = defaultExt;

	wi::helper::FileDialogParams params;
	params.type = wi::helper::FileDialogParams::SAVE;
	// Native first. Both are offered in the one filter the Win32 helper builds, so the
	// extension the user types still decides; this only sets what a bare name becomes.
	if (saveDefaultExt_ == "wiscene")
	{
		params.description = "Wicked scene archive";
		params.extensions.push_back("wiscene");
	}
	else
	{
		params.description = "Simtary scene descriptor";
		params.extensions.push_back("stsd");
		params.extensions.push_back("wiscene");
	}

	// The dialog runs on its own thread, so this callback is NOT on the main thread and
	// must not touch the scene. Park the path; Draw() serializes it next frame.
	wi::helper::FileDialog(params,
		[this](std::string fileName) {
			std::lock_guard<std::mutex> lock(pendingSaveMutex_);
			pendingSavePath_ = fileName;
			saveDialogOpen_ = false;
		},
		[this]() {
			saveDialogOpen_ = false;
		});
}

void st::EditorUI::FlushPendingSave(wi::scene::Scene& scene)
{
	std::string path;
	{
		std::lock_guard<std::mutex> lock(pendingSaveMutex_);
		if (pendingSavePath_.empty())
			return;
		path.swap(pendingSavePath_);
	}

	// The dialog does not append the extension when the user typed a bare name.
	if (wi::helper::GetExtensionFromFileName(path).empty())
		path += "." + saveDefaultExt_;

	SaveScene(scene, path);
}

// ----------------------------------------------------------------------- menus ---

void st::EditorUI::MenuItems()
{
	bool on = enabled_;
	if (ImGui::MenuItem("Editor Mode", "F2", &on))
		SetEnabled(on);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Dockable editor: viewports, hierarchy, properties, gizmo");

	ImGui::BeginDisabled(!enabled_);
	ImGui::MenuItem("Resource Explorer (editor)", nullptr, &showResources_);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Docked asset package browser / editor");
	if (ImGui::MenuItem("Reset Editor Layout"))
		resetLayout_ = true;
	ImGui::EndDisabled();
}

// ------------------------------------------------------------------------ draw ---

void st::EditorUI::Draw(App& app, wi::RenderPath3D& gamePath, Entity& selected)
{
	if (!enabled_)
		return;

	wi::scene::Scene& scene = wi::scene::GetScene();

	// Everything the render pass needs from the app, grabbed while we still have one.
	//	RenderEditorView / RenderCameraViews run later in the frame with no App& to hand.
	gfxSettings_    = &app.Graphics();
	gameColorSpace_ = gamePath.colorspace;
	gamePath_       = &gamePath;
	// The typed handle is the app's own path, which is the only one carrying the resolution
	//	override. A project that activated a path of its own in OnRenderPathSetup still gets
	//	the preview (that only needs wi::RenderPath3D), it just has no resolution control -
	//	there is nowhere to put the override on a path the framework does not own.
	gameRenderPath_ = (&app.renderPath == &gamePath) ? &app.renderPath : nullptr;
	ApplyGameViewResolution(true);

	// Main-thread tail of a Save As dialog that finished on its own thread.
	FlushPendingSave(scene);

	// Entity handles only mean anything inside one loaded scene: a scene switch invalidates
	//	every step we are holding, so drop them rather than replay them onto strangers.
	if (historySceneName_ != app.sceneManager.CurrentName())
	{
		historySceneName_ = app.sceneManager.CurrentName();
		history_.Clear();
		gizmoDragging_ = false;
	}

	pendingSelection_ = INVALID_ENTITY;

	// Main-thread tail of an import: a file dialog, or a model dropped on the window. The
	//	options panel is drawn at the very end of the frame, after the dock host, so it sits
	//	over the editor rather than under it.
	FlushPendingImport(scene);

	// Ctrl+S / Ctrl+Shift+S / Ctrl+Z / Ctrl+Y anywhere in the editor.
	const ImGuiIO& io = ImGui::GetIO();
	// gameViewFocused_ still holds last frame's value here (it is cleared further down), which
	// is what we want: while the game has focus its keys are the game's, editor chords included.
	if (io.KeyCtrl && !io.WantTextInput && !gameViewFocused_)
	{
		if (ImGui::IsKeyPressed(ImGuiKey_S, false))
		{
			if (io.KeyShift || scenePath_.empty())
				RequestSaveAs();
			else
				SaveScene(scene, scenePath_);
		}
		// Ctrl+Y and Ctrl+Shift+Z both redo; different editors trained different hands.
		if (ImGui::IsKeyPressed(ImGuiKey_Z, false))
			pendingSelection_ = io.KeyShift ? history_.Redo(scene) : history_.Undo(scene);
		if (ImGui::IsKeyPressed(ImGuiKey_Y, false))
			pendingSelection_ = history_.Redo(scene);
	}

	ImGuizmo::BeginFrame();

	DrawDockHost(app, scene);

	editorViewLive_    = false; // set again by the editor viewport if it is actually on screen
	freeCamLookWanted_ = false; // ditto for cursor ownership (see ReleaseFreeCamLook)
	gameViewFocused_   = false; // ...and for input ownership (see ReleaseInputCapture)
	gameViewHovered_   = false;

	if (showEditorViewport_ && editorPath_)
		DrawViewport(kEditorViewport, &showEditorViewport_, *editorPath_, editorCamera_, true, scene, selected);

	if (showGameViewport_)
	{
		CameraComponent& gameCam = gamePath.camera ? *gamePath.camera : wi::scene::GetCamera();
		DrawViewport(kGameViewport, &showGameViewport_, gamePath, gameCam, false, scene, selected);
	}

	DrawCameraViews(scene, selected);

	if (showHierarchy_)
	{
		if (ImGui::Begin(kHierarchy, &showHierarchy_))
			HierarchyGUI(scene, selected, &history_);
		ImGui::End();
	}

	if (showProperties_)
	{
		if (ImGui::Begin(kProperties, &showProperties_))
			PropertiesGUI(scene, selected, &history_);
		ImGui::End();
	}

	if (showResources_)
	{
		// The window is the editor's; the contents are the App's single AssetExplorer,
		// so an edit started in the floating DevUI window is still here.
		if (ImGui::Begin(kResources, &showResources_))
			app.Resources().GUI();
		ImGui::End();
	}

	// The import options panel, drawn last so the modal sits over the whole editor. It also
	//	has to come after the panels: pressing Import inside it builds entities, and doing that
	//	while the Hierarchy was mid-iteration would be editing the list it is walking.
	DrawImportOptions(scene);

	// A gizmo drag is one undo step, closed when ImGuizmo lets the mouse go.
	if (gizmoDragging_ && !ImGuizmo::IsUsingAny())
	{
		history_.PushTransform(scene, gizmoDragEntity_, GizmoLabel(), gizmoPreDrag_);
		gizmoDragging_ = false;
	}

	// Undo/redo and Create both nominate an entity to select; applying it here keeps the
	//	panels above from being handed a selection that changed halfway through the frame.
	if (pendingSelection_ != INVALID_ENTITY)
		selected = pendingSelection_;

	// Hand the (now final) selection to the render pass, which runs later in the frame with no
	// selection of its own.
	selectionHighlight_ = selected;

	// One place decides who owns the cursor, after every panel has had its say.
	if (freeCamLookWanted_ != freeCamLookActive_)
	{
		st::InputSystem::Get().SetUIMouseLook(freeCamLookWanted_);
		freeCamLookActive_ = freeCamLookWanted_;
	}

	// Input ownership: the game hears the keyboard and mouse only while its own viewport has
	//	focus. Everywhere else in the editor, st::Run stops feeding SDL events into wi::input
	//	entirely — which is what stops WASD typed at the editor viewport from also flying the
	//	game camera, including in game code that reads wi::input directly.
	// A game that has locked the cursor (an FPS look mode, say) has declared itself in
	//	control, and while it holds the pointer the user CANNOT click the Game Viewport to
	//	focus it — the cursor is hidden and warped. Treating that as "the game has input"
	//	breaks the deadlock; the game's own ESC handling is what gives it back.
	const bool gameOwnsCursor = st::InputSystem::Get().IsMouseCaptured();
	// ...and one exception to the exception: an open popup or menu owns the keyboard and the
	//	mouse while it is up. The Game Viewport has its own menu bar (resolution), and opening
	//	it focuses the Game Viewport window - so without this, dragging the Width field would
	//	also be typing at the game.
	const bool popupOpen = ImGui::IsPopupOpen(nullptr,
		ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
	const bool gameHasInput   = (gameViewFocused_ && !popupOpen) || gameOwnsCursor;

	const bool captureWanted = !gameHasInput;
	if (captureWanted != inputCaptureActive_)
	{
		st::InputSystem::Get().SetUIInputCapture(captureWanted);
		inputCaptureActive_ = captureWanted;
	}
	// ...and the other half: while the Game Viewport has focus, tell InputSystem to ignore
	//	ImGui's WantCapture* flags. Those flags are raised by the viewport panel itself - it
	//	is an ImGui window - so without this the panel whose job is to play the game is what
	//	suppresses the game's own keyboard and mouse.
	st::InputSystem::Get().SetGameViewportInput(gameHasInput, gameViewHovered_ || gameOwnsCursor);

	// A gizmo drag keeps a visible cursor, so it needs the other treatment: pin it inside
	//	the window for as long as the drag lasts, or a fast pull along an axis walks the
	//	pointer onto the desktop (or a second monitor) while the drag is still live.
	const bool confineWanted = ImGuizmo::IsUsingAny();
	if (confineWanted != gizmoConfineActive_)
	{
		st::InputSystem::Get().SetUIMouseConfined(confineWanted);
		gizmoConfineActive_ = confineWanted;
	}
}

void st::EditorUI::ReleaseInputCapture()
{
	// No early-out on inputCaptureActive_: the game-viewport override is a separate flag and
	//	has to be cleared even when the capture itself was never raised.
	st::InputSystem::Get().SetUIInputCapture(false);
	st::InputSystem::Get().SetGameViewportInput(false, false);
	inputCaptureActive_ = false;
	gameViewFocused_ = false;
	gameViewHovered_ = false;
}

void st::EditorUI::ReleaseFreeCamLook()
{
	if (gizmoConfineActive_)
	{
		st::InputSystem::Get().SetUIMouseConfined(false);
		gizmoConfineActive_ = false;
	}
	if (!freeCamLookActive_)
		return;
	st::InputSystem::Get().SetUIMouseLook(false);
	freeCamLookActive_ = false;
	freeCamLookWanted_ = false;
}

// ------------------------------------------------------------- editor viewport ---

void st::EditorUI::RenderEditorView(float dt)
{
	if (!enabled_ || !editorPath_ || !editorViewLive_)
		return;
	if (editorViewSize_.x < 16 || editorViewSize_.y < 16)
		return;

	// Follow the panel. RenderPath2D::Update reallocates when the size actually changed.
	editorPath_->init(editorViewSize_.x, editorViewSize_.y, editorViewDPI_);

	// Build the camera AFTER the resize: RenderPath3D::ResizeBuffers writes
	// camera->width/height from the internal resolution, and UpdateCamera derives the
	// projection aspect from those.
	TransformComponent camTransform;
	camTransform.translation_local = camPos_;
	camTransform.RotateRollPitchYaw(camRot_);
	camTransform.UpdateTransform();

	editorPath_->PreUpdate();
	editorPath_->Update(dt);
	editorPath_->PostUpdate();

	editorCamera_.TransformCamera(camTransform);
	editorCamera_.fov    = camFov_;
	editorCamera_.zNearP = camNear_;
	editorCamera_.zFarP  = camFar_;
	editorCamera_.UpdateCamera();

	ApplyViewportRenderer(*editorPath_, showGamePreview_);

	// Debug draws, editor viewport only. The scope restores the globals afterwards, and the
	//	game path already rendered this frame, so the Game Viewport never sees any of it.
	DebugFlagScope debugScope;
	wi::renderer::SetToDrawDebugCameras(debug_.cameras);
	wi::renderer::SetToDrawDebugColliders(debug_.colliders);
	wi::renderer::SetToDrawDebugEmitters(debug_.emitters);
	wi::renderer::SetToDrawDebugEnvProbes(debug_.envProbes);
	wi::renderer::SetToDrawDebugForceFields(debug_.forceFields);
	wi::renderer::SetToDrawDebugSprings(debug_.springs);
	wi::renderer::SetToDrawDebugBoneLines(debug_.boneLines);
	wi::renderer::SetToDrawDebugPartitionTree(debug_.partitionTree);
	wi::renderer::SetToDrawGridHelper(debug_.grid);
	wi::renderer::SetToDrawVoxelHelper(debug_.voxels, debug_.voxelClipmap);

	if (debug_.gameCamera)
		QueueCameraFrustum(wi::scene::GetCamera(), XMFLOAT4(1.0f, 0.85f, 0.2f, 1.0f));

	if (debug_.selection)
		QueueSelectionHighlight(wi::scene::GetScene(), selectionHighlight_);

	editorPath_->PreRender();
	editorPath_->Render();
	editorPath_->PostRender();
}

void st::EditorUI::ApplyViewportRenderer(wi::RenderPath3D& path, bool gamePreview) const
{
	if (!gamePreview)
	{
		ApplyEngineDefaults(path);
		return;
	}

	// The engine assigns colorspace to the ACTIVE path only (wiApplication.cpp), so an
	//	editor-owned path keeps the SRGB default. On an HDR10 swapchain that alone sends the
	//	tonemapper down a different branch and the picture comes out a different colour.
	path.colorspace = gameColorSpace_;

	if (gfxSettings_ != nullptr)
		gfxSettings_->MirrorTo(path, stableExposure_);

	// The rest of "the same renderer", which is not a graphics setting:
	//
	//	layer mask     a game that keeps a layer off the player's screen keeps it off this
	//	               one too, otherwise the preview shows more than the game does.
	//	custom passes  st::ProjectorSystem and st::LaserSystem both hang their compute pass
	//	               on RenderPath3D::custom_post_processes, and each system Bind()s
	//	               exactly ONE path - so the preview cannot be hooked, it has to be
	//	               handed a copy of the game path's list. What the passes read from that
	//	               copy is view independent: params0 carries the descriptor index of the
	//	               projector/laser upload buffer and the count, both written by
	//	               ProjectorSystem::Update earlier this frame, and the projector shadow
	//	               maps they sample were recorded by the game path's own postprocess
	//	               chain - which has already run by the time an editor path renders.
	if (gamePath_ != nullptr)
	{
		path.setlayerMask(gamePath_->getLayerMask());
		path.custom_post_processes = gamePath_->custom_post_processes;
	}
}

// -------------------------------------------------- game viewport resolution ---

void st::EditorUI::ApplyGameViewResolution(bool enabled)
{
	if (gameRenderPath_ == nullptr)
		return;

	// Editor-only, by construction: leaving editor mode (or shutting down) puts the game
	//	path back on the window canvas, so a resolution picked here can never follow the
	//	build out of the editor and letterbox a shipping frame.
	if (!enabled || !gameRes_.fixed)
	{
		gameRenderPath_->forcedResolution = XMUINT2(0, 0);
		return;
	}

	gameRenderPath_->forcedResolution = XMUINT2(
		(uint32_t)std::max(16, gameRes_.width),
		(uint32_t)std::max(16, gameRes_.height));
}

// -------------------------------------------------------------- camera views ---

namespace {

std::string CameraLabel(wi::scene::Scene& scene, Entity e)
{
	if (e == INVALID_ENTITY)
		return "(no camera)";
	const wi::scene::NameComponent* n = scene.names.GetComponent(e);
	if (n != nullptr && !n->name.empty())
		return n->name + "  [" + std::to_string(e) + "]";
	return "Entity " + std::to_string(e);
}

} // namespace

void st::EditorUI::SpawnCameraView(Entity camera)
{
	auto view = std::make_unique<CameraView>();
	view->id = nextCameraViewID_++;
	view->camera = camera;

	view->path = std::make_unique<wi::RenderPath3D>();
	view->path->camera = &view->cam; // stable address: CameraView lives behind a unique_ptr
	// Same contract as the other editor paths: the scene is stepped once per frame by the
	//	scene itself, and occlusion-culling history is meaningless for a path that only
	//	renders while its panel happens to be on screen.
	view->path->setSceneUpdateEnabled(false);
	view->path->setOcclusionCullingEnabled(false);
	view->path->setMSAASampleCount(1);
	view->path->init((uint32_t)view->width, (uint32_t)view->height, 96.0f);
	view->path->Start();

	cameraViews_.push_back(std::move(view));
}

void st::EditorUI::DestroyCameraViews()
{
	if (cameraViews_.empty())
		return;
	wi::graphics::GetDevice()->WaitForGPU(); // render targets may still be in flight
	cameraViews_.clear();
}

void st::EditorUI::DrawCameraViewToolbar(wi::scene::Scene& scene, CameraView& view)
{
	if (!ImGui::BeginMenuBar())
		return;

	// -- which camera --
	if (ImGui::BeginMenu(CameraLabel(scene, view.camera).c_str()))
	{
		if (scene.cameras.GetCount() == 0)
			ImGui::TextDisabled("no Camera components in this scene");
		for (size_t i = 0; i < scene.cameras.GetCount(); ++i)
		{
			const Entity e = scene.cameras.GetEntity(i);
			if (ImGui::MenuItem(CameraLabel(scene, e).c_str(), nullptr, view.camera == e))
				view.camera = e;
		}
		ImGui::EndMenu();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Which camera this view renders through");

	// -- renderer --
	ImGui::Separator();
	ImGui::Checkbox("Show Game Preview", &view.showGamePreview);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Render this view through the game's own renderer - the live "
			"graphics settings (AO, bloom, tonemap, exposure, colour grading), the game "
			"path's colorspace and layer mask, and its custom post process passes "
			"(projectors, lasers). Off falls back to bare RenderPath3D defaults.\n\n"
			"Per view: one window can sit on the shipping look while another shows flat "
			"geometry.");

	// -- resolution + aspect --
	ImGui::Separator();
	ImGui::Checkbox("Fit", &view.matchPanel);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Render at the panel's pixel size instead of a fixed resolution");

	ImGui::BeginDisabled(view.matchPanel);
	DrawResolutionMenus(view.width, view.height, view.ratio);
	ImGui::EndDisabled();

	ImGui::Separator();
	ImGui::TextDisabled("%d x %d  %.3f", view.width, view.height,
		(float)view.width / (float)std::max(1, view.height));

	// Right-aligned reminder of what this window is.
	{
		const char* note = "view only";
		const float w = ImGui::CalcTextSize(note).x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
		if (ImGui::GetCursorPosX() < ImGui::GetWindowWidth() - w)
			ImGui::SetCursorPosX(ImGui::GetWindowWidth() - w);
		ImGui::TextDisabled("%s", note);
	}

	ImGui::EndMenuBar();
}

void st::EditorUI::DrawCameraViews(wi::scene::Scene& scene, Entity selected)
{
	for (auto& viewPtr : cameraViews_)
	{
		CameraView& view = *viewPtr;
		view.live = false;

		// A camera this view was pointed at can be deleted out from under it.
		if (view.camera != INVALID_ENTITY && !scene.cameras.Contains(view.camera))
			view.camera = INVALID_ENTITY;

		// "Camera View 3" is the visible title; the ###id keeps the window identity - and so
		//	its docked position and size in imgui.ini - stable regardless of the label.
		const std::string title = "Camera View " + std::to_string(view.id) +
			"###simtary_camview_" + std::to_string(view.id);

		ImGui::SetNextWindowSize(ImVec2(520, 340), ImGuiCond_FirstUseEver);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		const bool open = ImGui::Begin(title.c_str(), &view.open, ImGuiWindowFlags_MenuBar);
		ImGui::PopStyleVar();
		if (!open)
		{
			ImGui::End();
			continue;
		}

		DrawCameraViewToolbar(scene, view);

		const ImVec2 avail  = ImGui::GetContentRegionAvail();
		const ImVec2 cursor = ImGui::GetCursorScreenPos();
		const bool   usable = (avail.x >= 16.0f && avail.y >= 16.0f);

		if (view.matchPanel && usable)
		{
			view.width  = (int)avail.x;
			view.height = (int)avail.y;
		}
		view.live = usable && view.path && view.camera != INVALID_ENTITY;

		const wi::graphics::Texture* tex = view.path ? view.path->GetLastPostprocessRT() : nullptr;
		if (view.live && tex != nullptr && tex->IsValid())
		{
			// Letterbox. The whole point of this window is that the framing follows the
			//	chosen resolution, not the shape the dock happens to give it.
			const wi::graphics::TextureDesc& d = tex->GetDesc();
			const float texAspect   = (float)d.width / (float)std::max(1u, d.height);
			const float panelAspect = avail.x / avail.y;
			const ImVec2 imageSize = (panelAspect > texAspect)
				? ImVec2(avail.y * texAspect, avail.y)
				: ImVec2(avail.x, avail.x / texAspect);
			const ImVec2 imagePos(cursor.x + (avail.x - imageSize.x) * 0.5f,
				cursor.y + (avail.y - imageSize.y) * 0.5f);

			ImGui::GetWindowDrawList()->AddRectFilled(cursor,
				ImVec2(cursor.x + avail.x, cursor.y + avail.y), IM_COL32(0, 0, 0, 255));
			ImGui::SetCursorScreenPos(imagePos);
			// Deliberately ImGui::Image and nothing else: no InvisibleButton, no gizmo, no
			//	picking, no camera control. This window is a monitor.
			ImGui::Image((ImTextureID)(uintptr_t)tex, imageSize);
		}
		else
		{
			ImGui::Dummy(avail);
			ImGui::SetCursorScreenPos(ImVec2(cursor.x + 10, cursor.y + 10));
			if (view.camera == INVALID_ENTITY)
			{
				ImGui::TextDisabled("Pick a camera from the menu above.");
				if (selected != INVALID_ENTITY && scene.cameras.Contains(selected))
				{
					ImGui::SetCursorScreenPos(ImVec2(cursor.x + 10, cursor.y + 34));
					if (ImGui::Button("Use the selected camera"))
						view.camera = selected;
				}
			}
			else
			{
				ImGui::TextDisabled("waiting for the first frame...");
			}
		}

		ImGui::End();
	}

	// Drop closed views. Their render targets may still be referenced by a frame in flight,
	//	so this waits for the GPU - closing a window is rare enough for that to be fine.
	bool anyClosed = false;
	for (auto& v : cameraViews_)
		anyClosed |= !v->open;
	if (anyClosed)
	{
		wi::graphics::GetDevice()->WaitForGPU();
		cameraViews_.erase(
			std::remove_if(cameraViews_.begin(), cameraViews_.end(),
				[](const std::unique_ptr<CameraView>& v) { return !v->open; }),
			cameraViews_.end());
	}
}

void st::EditorUI::RenderCameraViews(float dt)
{
	if (!enabled_ || cameraViews_.empty())
		return;

	wi::scene::Scene& scene = wi::scene::GetScene();

	for (auto& viewPtr : cameraViews_)
	{
		CameraView& view = *viewPtr;
		if (!view.live || !view.path || view.camera == INVALID_ENTITY)
			continue;

		const CameraComponent* src = scene.cameras.GetComponent(view.camera);
		if (src == nullptr)
			continue;

		// Copy the scene camera's view state. Scene::RunCameraUpdateSystem already pulled it
		//	from the entity's transform this frame, so Eye/At/Up are current. It is a copy and
		//	not a pointer because RenderPath3D::ResizeBuffers writes camera->width/height -
		//	pointing at the scene's own camera would change what the game renders with.
		view.cam = *src;

		ApplyViewportRenderer(*view.path, view.showGamePreview);

		view.path->init((uint32_t)view.width, (uint32_t)view.height, 96.0f);
		view.path->PreUpdate();
		view.path->Update(dt);   // resizes the targets and writes camera->width/height
		view.path->PostUpdate();

		view.cam.UpdateCamera();  // rebuild the projection at this view's own aspect

		view.path->PreRender();
		view.path->Render();
		view.path->PostRender();
	}
}

