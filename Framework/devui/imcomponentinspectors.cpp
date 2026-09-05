#include "imcomponentinspectors.h"

#include "imcomponents.h"
#include "imassets.h"
#include "imeditorhistory.h"

#include "wiScene.h"
#include "wiMath.h"
#include "wiEnums.h"
#include "imgui.h"

#include <cfloat>
#include <cstdio>
#include <string>
#include <vector>

using wi::ecs::Entity;
using wi::ecs::INVALID_ENTITY;
using namespace wi::scene;

namespace st::devui {

// helpers

std::string EntityLabel(Scene& scene, Entity e)
{
	if (e == INVALID_ENTITY) return "(none)";
	if (const NameComponent* n = scene.names.GetComponent(e); n && !n->name.empty())
		return n->name;
	return "Entity " + std::to_string((unsigned)e);
}

bool EditString(const char* label, std::string& s)
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

// A string field naming an ASSET (a texture, a sound, a script, a sky map) plus a drop
//	target for the Resource Explorer. Dropping a row from that window writes its LOGICAL
//	path ("textures/wall.dds"), which is exactly the spelling a mounted package resolves,
//	so a packed asset and a loose one are named the same way here.
//
//	Returns true only when the value changed by DROP, because a drop is one complete change
//	while a keystroke is a third of a filename: callers apply a drop at once and apply typed
//	edits on IsItemDeactivatedAfterEdit.
bool AssetDropField(const char* label, std::string& value)
{
	EditString(label, value);

	bool dropped = false;
	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(st::SIMTARY_ASSET_PAYLOAD))
		{
			if (p->DataSize == (int)sizeof(st::AssetPayload))
			{
				const st::AssetPayload* a = (const st::AssetPayload*)p->Data;
				value = a->path;
				dropped = true;
			}
		}
		ImGui::EndDragDropTarget();
	}
	return dropped;
}

bool ComponentHeader(Scene& scene, Entity e, const char* libraryKey,
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

namespace {

constexpr float kDeg = 180.0f / XM_PI;
constexpr float kRad = XM_PI / 180.0f;

// One undo step per widget interaction, not one per frame. The step opens when ImGui makes
//	the item active and closes when the item goes inactive HAVING changed something, so a
//	drag that spans forty frames is one step and a drag that ends where it started is none.
struct Edits
{
	Scene& scene;
	Entity entity;
	st::EditorHistory* history;

	void operator()(const char* label) const
	{
		if (history == nullptr) return;
		if (ImGui::IsItemActivated())            history->BeginEntity(scene, entity, label);
		if (ImGui::IsItemDeactivatedAfterEdit()) history->Commit(scene);
		else if (ImGui::IsItemDeactivated())     history->Abort();
	}
};

// Checkbox bound to an Is.../Set... flag pair. Returns true on the frame it toggled.
template <typename Getter, typename Setter>
bool FlagCheckbox(const char* label, Getter get, Setter set)
{
	bool v = get();
	if (ImGui::Checkbox(label, &v)) { set(v); return true; }
	return false;
}

// Angle stored in radians, edited in degrees.
bool DragAngle(const char* label, float& radians, float speed = 0.25f,
	float minDeg = -360.0f, float maxDeg = 360.0f)
{
	float deg = radians * kDeg;
	if (ImGui::DragFloat(label, &deg, speed, minDeg, maxDeg, "%.2f deg"))
	{
		radians = deg * kRad;
		return true;
	}
	return false;
}

bool DragUint(const char* label, uint32_t& v, float speed = 1.0f)
{
	const uint32_t lo = 0;
	return ImGui::DragScalar(label, ImGuiDataType_U32, &v, speed, &lo, nullptr);
}

// A read-only "which entity does this point at" row. The panel's drag-drop EntityField is
//	bound to NATIVE component parameters (it writes GUID metadata that survives a reload);
//	an engine component's handle is a raw runtime ID, so it is shown rather than re-bound.
void EntityRef(Scene& scene, const char* label, Entity e)
{
	ImGui::LabelText(label, "%s", EntityLabel(scene, e).c_str());
}

// 32-bit mask as hex, which is the only spelling that reads back.
bool EditMask(const char* label, uint32_t& mask)
{
	int v = (int)mask;
	if (ImGui::InputScalar(label, ImGuiDataType_S32, &v, nullptr, nullptr, "%08X",
		ImGuiInputTextFlags_CharsHexadecimal))
	{
		mask = (uint32_t)v;
		return true;
	}
	return false;
}

void HelpMarker(const char* text)
{
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered())
	{
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
		ImGui::TextUnformatted(text);
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}

// layer

void DrawLayer(Scene& scene, Entity e, st::EditorHistory* history)
{
	LayerComponent* c = scene.layers.GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::layers", "Layer", false, history)) return;
	const Edits track{ scene, e, history };

	EditMask("Layer mask", c->layerMask);
	track("Set Layer Mask");
	HelpMarker("Serialized. Culling, picking and light/decal filtering all test this mask.");

	ImGui::BeginDisabled(true);
	uint32_t propagation = c->propagationMask;
	EditMask("Propagation mask", propagation);
	ImGui::EndDisabled();
	HelpMarker("Not serialized: the hierarchy system rewrites it from the parent chain every "
		"frame. Shown because a child's effective mask is layerMask & this.");

	ImGui::Text("Effective: %08X", c->GetLayerMask());
}

// hierarchy

void DrawHierarchy(Scene& scene, Entity e, st::EditorHistory* history)
{
	HierarchyComponent* c = scene.hierarchy.GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::hierarchy", "Hierarchy", false, history)) return;
	const Edits track{ scene, e, history };

	EntityRef(scene, "Parent", c->parentID);
	HelpMarker("Re-parent by dragging the row onto another row in the Hierarchy panel: "
		"Component_Attach has to rebase the child transform, which a raw write of this field "
		"would skip.");

	EditMask("Bound layer mask", c->layerMask_bind);
	track("Set Bound Layer Mask");
	HelpMarker("The child's own layer mask at the moment it was attached. The hierarchy system "
		"ANDs it with the parent's to produce the propagation mask.");

	if (ImGui::Button("Detach from parent"))
	{
		if (history) history->BeginEntity(scene, e, "Detach From Parent");
		scene.Component_Detach(e);
		if (history) history->Commit(scene);
	}
}

// material

void DrawMaterial(Scene& scene, Entity e, st::EditorHistory* history)
{
	MaterialComponent* m = scene.materials.GetComponent(e);
	if (!m) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::materials", "Material", true, history)) return;
	const Edits track{ scene, e, history };

	bool dirty = false;

	static const char* shaderTypes[] = {
		"PBR", "PBR + planar reflection", "PBR + parallax occlusion mapping",
		"PBR anisotropic", "Water", "Cartoon", "Unlit", "PBR cloth",
		"PBR clearcoat", "PBR cloth + clearcoat", "PBR terrain blended", "Interior mapping",
	};
	int shader = (int)m->shaderType;
	if (ImGui::Combo("Shader", &shader, shaderTypes, IM_ARRAYSIZE(shaderTypes)))
	{
		m->shaderType = (MaterialComponent::SHADERTYPE)shader;
		dirty = true;
	}
	track("Set Shader Type");

	static const char* blendModes[] = { "Opaque", "Alpha", "Premultiplied", "Additive", "Multiply", "Inverse" };
	int blend = (int)m->userBlendMode;
	if (ImGui::Combo("Blend mode", &blend, blendModes, IM_ARRAYSIZE(blendModes)))
	{
		m->userBlendMode = (wi::enums::BLENDMODE)blend;
		dirty = true;
	}
	track("Set Blend Mode");

	if (ImGui::TreeNodeEx("Colors", ImGuiTreeNodeFlags_DefaultOpen))
	{
		dirty |= ImGui::ColorEdit4("Base color", &m->baseColor.x);
		track("Set Base Color");
		dirty |= ImGui::DragFloat("Opacity", &m->baseColor.w, 0.005f, 0.0f, 1.0f);
		track("Set Opacity");
		dirty |= ImGui::ColorEdit4("Specular color", &m->specularColor.x);
		track("Set Specular Color");
		dirty |= ImGui::ColorEdit3("Emissive", &m->emissiveColor.x);
		track("Set Emissive Color");
		dirty |= ImGui::DragFloat("Emissive strength", &m->emissiveColor.w, 0.01f, 0.0f, 1000.0f);
		track("Set Emissive Strength");
		dirty |= ImGui::ColorEdit3("Subsurface scattering", &m->subsurfaceScattering.x);
		track("Set SSS Color");
		dirty |= ImGui::DragFloat("SSS amount", &m->subsurfaceScattering.w, 0.005f, 0.0f, 1.0f);
		track("Set SSS Amount");
		dirty |= ImGui::ColorEdit4("Extinction color", &m->extinctionColor.x);
		track("Set Extinction Color");
		dirty |= ImGui::DragFloat("Saturation", &m->saturation, 0.005f, 0.0f, 2.0f);
		track("Set Saturation");
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Surface", ImGuiTreeNodeFlags_DefaultOpen))
	{
		dirty |= ImGui::DragFloat("Roughness", &m->roughness, 0.005f, 0.0f, 1.0f);
		track("Set Roughness");
		dirty |= ImGui::DragFloat("Metalness", &m->metalness, 0.005f, 0.0f, 1.0f);
		track("Set Metalness");
		dirty |= ImGui::DragFloat("Reflectance", &m->reflectance, 0.005f, 0.0f, 1.0f);
		track("Set Reflectance");
		dirty |= ImGui::DragFloat("Normal map strength", &m->normalMapStrength, 0.01f, 0.0f, 8.0f);
		track("Set Normal Map Strength");
		dirty |= ImGui::DragFloat("Parallax occlusion", &m->parallaxOcclusionMapping, 0.001f, 0.0f, 0.1f);
		track("Set Parallax Occlusion");
		dirty |= ImGui::DragFloat("Displacement", &m->displacementMapping, 0.01f, 0.0f, 10.0f);
		track("Set Displacement");
		dirty |= ImGui::DragFloat("Alpha ref", &m->alphaRef, 0.005f, 0.0f, 1.0f);
		track("Set Alpha Ref");
		HelpMarker("Alpha testing turns on as soon as this drops below 1.");
		dirty |= ImGui::DragFloat("Mesh blend", &m->mesh_blend, 0.005f, 0.0f, 1.0f);
		track("Set Mesh Blend");
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Transparency / refraction"))
	{
		dirty |= ImGui::DragFloat("Refraction", &m->refraction, 0.005f, 0.0f, 1.0f);
		track("Set Refraction");
		dirty |= ImGui::DragFloat("Transmission", &m->transmission, 0.005f, 0.0f, 1.0f);
		track("Set Transmission");
		dirty |= ImGui::DragFloat("Cloak", &m->cloak, 0.005f, 0.0f, 1.0f);
		track("Set Cloak");
		dirty |= ImGui::DragFloat("Chromatic aberration", &m->chromatic_aberration, 0.005f, 0.0f, 1.0f);
		track("Set Chromatic Aberration");
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Anisotropy / sheen / clearcoat"))
	{
		dirty |= ImGui::DragFloat("Anisotropy strength", &m->anisotropy_strength, 0.005f, 0.0f, 1.0f);
		track("Set Anisotropy Strength");
		dirty |= DragAngle("Anisotropy rotation", m->anisotropy_rotation);
		track("Set Anisotropy Rotation");
		dirty |= ImGui::ColorEdit3("Sheen color", &m->sheenColor.x);
		track("Set Sheen Color");
		dirty |= ImGui::DragFloat("Sheen roughness", &m->sheenRoughness, 0.005f, 0.0f, 1.0f);
		track("Set Sheen Roughness");
		dirty |= ImGui::DragFloat("Clearcoat", &m->clearcoat, 0.005f, 0.0f, 1.0f);
		track("Set Clearcoat");
		dirty |= ImGui::DragFloat("Clearcoat roughness", &m->clearcoatRoughness, 0.005f, 0.0f, 1.0f);
		track("Set Clearcoat Roughness");
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("UV / texture animation"))
	{
		dirty |= ImGui::DragFloat2("Tex multiply", &m->texMulAdd.x, 0.01f);
		track("Set Tex Multiply");
		dirty |= ImGui::DragFloat2("Tex add", &m->texMulAdd.z, 0.01f);
		track("Set Tex Add");
		dirty |= ImGui::DragFloat2("Anim direction", &m->texAnimDirection.x, 0.001f);
		track("Set Tex Anim Direction");
		dirty |= ImGui::DragFloat("Anim frame rate", &m->texAnimFrameRate, 0.1f, 0.0f, 240.0f);
		track("Set Tex Anim Frame Rate");
		ImGui::Text("Elapsed: %.3f s", m->texAnimElapsedTime);
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Terrain / interior mapping"))
	{
		dirty |= ImGui::DragFloat("Blend with terrain height", &m->blend_with_terrain_height, 0.01f, 0.0f, 1000.0f);
		track("Set Terrain Blend Height");
		dirty |= DragAngle("Interior rotation", m->interiorMappingRotation);
		track("Set Interior Rotation");
		dirty |= ImGui::DragFloat3("Interior scale", &m->interiorMappingScale.x, 0.01f);
		track("Set Interior Scale");
		dirty |= ImGui::DragFloat3("Interior offset", &m->interiorMappingOffset.x, 0.01f);
		track("Set Interior Offset");
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Flags", ImGuiTreeNodeFlags_DefaultOpen))
	{
		dirty |= FlagCheckbox("Cast shadow", [&] { return m->IsCastingShadow(); },
			[&](bool v) { m->SetCastShadow(v); });
		dirty |= FlagCheckbox("Receive shadow", [&] { return m->IsReceiveShadow(); },
			[&](bool v) { m->SetReceiveShadow(v); });
		dirty |= FlagCheckbox("Double sided", [&] { return m->IsDoubleSided(); },
			[&](bool v) { m->SetDoubleSided(v); });
		dirty |= FlagCheckbox("Outline", [&] { return m->IsOutlineEnabled(); },
			[&](bool v) { m->SetOutlineEnabled(v); });
		dirty |= FlagCheckbox("Use vertex colors", [&] { return m->IsUsingVertexColors(); },
			[&](bool v) { m->SetUseVertexColors(v); });
		dirty |= FlagCheckbox("Use wind", [&] { return m->IsUsingWind(); },
			[&](bool v) { m->SetUseWind(v); });
		dirty |= FlagCheckbox("Specular-glossiness workflow",
			[&] { return m->IsUsingSpecularGlossinessWorkflow(); },
			[&](bool v) { m->SetUseSpecularGlossinessWorkflow(v); });
		dirty |= FlagCheckbox("Occlusion: primary", [&] { return m->IsOcclusionEnabled_Primary(); },
			[&](bool v) { m->SetOcclusionEnabled_Primary(v); });
		dirty |= FlagCheckbox("Occlusion: secondary", [&] { return m->IsOcclusionEnabled_Secondary(); },
			[&](bool v) { m->SetOcclusionEnabled_Secondary(v); });
		dirty |= FlagCheckbox("Disable vertex AO", [&] { return m->IsVertexAODisabled(); },
			[&](bool v) { m->SetVertexAODisabled(v); });
		dirty |= FlagCheckbox("Disable texture streaming", [&] { return m->IsTextureStreamingDisabled(); },
			[&](bool v) { m->SetTextureStreamingDisabled(v); });
		dirty |= FlagCheckbox("Coplanar blending", [&] { return m->IsCoplanarBlending(); },
			[&](bool v) { m->SetCoplanarBlending(v); });
		dirty |= FlagCheckbox("Disable capsule shadow", [&] { return m->IsCapsuleShadowDisabled(); },
			[&](bool v) { m->SetCapsuleShadowDisabled(v); });
		// Deliberately not folded into `dirty`: this setter recreates the texture resources
		// itself, and a SetDirty() on top would ask for the work twice.
		FlagCheckbox("Prefer uncompressed textures",
			[&] { return m->IsPreferUncompressedTexturesEnabled(); },
			[&](bool v) { m->SetPreferUncompressedTexturesEnabled(v); });
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Stencil / shading rate"))
	{
		int engineRef = (int)m->engineStencilRef;
		if (ImGui::SliderInt("Engine stencil ref", &engineRef, 0, 15))
		{
			m->engineStencilRef = (wi::enums::STENCILREF)engineRef;
			dirty = true;
		}
		track("Set Engine Stencil Ref");
		HelpMarker("0 empty, 1 default, 2 custom shader, 3 outline, 4 custom shader + outline.");

		int userRef = (int)m->userStencilRef;
		if (ImGui::SliderInt("User stencil ref", &userRef, 0, 15))
		{
			m->SetUserStencilRef((uint8_t)userRef);
			dirty = true;
		}
		track("Set User Stencil Ref");

		static const char* rates[] = { "1x1", "1x2", "2x1", "2x2", "2x4", "4x2", "4x4" };
		int rate = (int)m->shadingRate;
		if (rate >= 0 && rate < IM_ARRAYSIZE(rates) &&
			ImGui::Combo("Shading rate", &rate, rates, IM_ARRAYSIZE(rates)))
		{
			m->shadingRate = (wi::graphics::ShadingRate)rate;
			dirty = true;
		}
		track("Set Shading Rate");
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Textures", ImGuiTreeNodeFlags_DefaultOpen))
	{
		static const char* slotNames[MaterialComponent::TEXTURESLOT_COUNT] = {
			"Base color", "Normal", "Surface", "Emissive", "Displacement",
			"Occlusion", "Transmission", "Sheen color", "Sheen roughness",
			"Clearcoat", "Clearcoat roughness", "Clearcoat normal",
			"Specular", "Anisotropy", "Transparency",
		};
		ImGui::TextDisabled("Drag a row out of the Resource Explorer onto a File field.");
		for (int i = 0; i < MaterialComponent::TEXTURESLOT_COUNT; ++i)
		{
			MaterialComponent::TextureMap& slot = m->textures[i];
			ImGui::PushID(i);
			if (ImGui::TreeNodeEx(slotNames[i], slot.name.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen))
			{
				// The resource is bound by NAME, so a new name is only a new texture once it
				//	has been loaded. That load runs on COMMIT (a drop, or focus leaving the
				//	field) and never per keystroke: CreateRenderData re-resolves all fifteen
				//	slots, which is fifteen lookups for every letter of a typed filename.
				if (AssetDropField("File", slot.name))
				{
					if (history) history->BeginEntity(scene, e, "Set Texture");
					m->CreateRenderData(true);
					m->SetDirty();
					if (history) history->Commit(scene);
				}
				else
				{
					if (ImGui::IsItemDeactivatedAfterEdit())
					{
						m->CreateRenderData(true);
						dirty = true;
					}
					track("Set Texture");
				}

				int uvset = (int)slot.uvset;
				if (ImGui::SliderInt("UV set", &uvset, 0, 1))
				{
					slot.uvset = (uint32_t)uvset;
					dirty = true;
				}
				track("Set Texture UV Set");

				ImGui::TextDisabled(slot.resource.IsValid() ? "loaded" : "not loaded");
				if (!slot.name.empty())
				{
					ImGui::SameLine();
					if (ImGui::SmallButton("Clear"))
					{
						if (history) history->BeginEntity(scene, e, "Clear Texture");
						slot.name.clear();
						slot.resource = {};
						m->CreateRenderData(true);
						m->SetDirty();
						if (history) history->Commit(scene);
					}
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Advanced"))
	{
		int customShader = m->customShaderID;
		if (ImGui::InputInt("Custom shader ID", &customShader))
		{
			m->customShaderID = customShader;
			dirty = true;
		}
		track("Set Custom Shader");
		HelpMarker("-1 disables. Indexes the renderer's custom shader registry.");

		int userdata[4] = { (int)m->userdata.x, (int)m->userdata.y, (int)m->userdata.z, (int)m->userdata.w };
		if (ImGui::InputInt4("User data", userdata))
		{
			m->userdata = uint4((uint32_t)userdata[0], (uint32_t)userdata[1],
				(uint32_t)userdata[2], (uint32_t)userdata[3]);
			dirty = true;
		}
		track("Set Material User Data");

		EntityRef(scene, "Camera source", m->cameraSource);
		HelpMarker("When set, the base colour is taken from that camera's render target.");

		ImGui::Text("Filter mask: %08X", m->GetFilterMask());
		ImGui::Text("Stencil ref: %u", (unsigned)m->GetStencilRef());
		ImGui::TreePop();
	}

	if (dirty)
		m->SetDirty();
}

// mesh

void DrawMesh(Scene& scene, Entity e, st::EditorHistory* history)
{
	MeshComponent* m = scene.meshes.GetComponent(e);
	if (!m) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::meshes", "Mesh", false, history)) return;
	const Edits track{ scene, e, history };

	ImGui::Text("%zu vertices, %zu indices, %zu subsets",
		m->vertex_positions.size(), m->indices.size(), m->subsets.size());
	ImGui::TextDisabled("Geometry is imported, not authored here.");

	ImGui::DragFloat("Tessellation factor", &m->tessellationFactor, 0.01f, 0.0f, 64.0f);
	track("Set Tessellation Factor");

	DragUint("Subsets per LOD", m->subsets_per_lod);
	track("Set Subsets Per LOD");
	HelpMarker("Non-zero declares LOD levels: subsets [n*count, (n+1)*count) are LOD n.");

	EntityRef(scene, "Armature", m->armatureID);
	ImGui::Text("Skinned: %s", m->IsSkinned() ? "yes" : "no");

	FlagCheckbox("Renderable", [&] { return m->IsRenderable(); }, [&](bool v) { m->SetRenderable(v); });
	FlagCheckbox("Double sided", [&] { return m->IsDoubleSided(); }, [&](bool v) { m->SetDoubleSided(v); });
	FlagCheckbox("Double sided shadow", [&] { return m->IsDoubleSidedShadow(); },
		[&](bool v) { m->SetDoubleSidedShadow(v); });
	FlagCheckbox("Dynamic", [&] { return m->IsDynamic(); }, [&](bool v) { m->SetDynamic(v); });
	HelpMarker("Dynamic meshes get a streamout buffer, which is what soft bodies and "
		"GPU-skinned morph targets write into.");
	FlagCheckbox("BVH enabled", [&] { return m->IsBVHEnabled(); }, [&](bool v) { m->SetBVHEnabled(v); });
	HelpMarker("Builds a CPU BVH for this mesh, which is what makes per-triangle raycasts "
		"against it cheap. Ticking it builds the tree now.");
	FlagCheckbox("Disable quantized positions", [&] { return m->IsQuantizedPositionsDisabled(); },
		[&](bool v) { m->SetQuantizedPositionsDisabled(v); });

	if (!m->subsets.empty() && ImGui::TreeNode("Subsets"))
	{
		for (size_t i = 0; i < m->subsets.size(); ++i)
		{
			MeshComponent::MeshSubset& s = m->subsets[i];
			ImGui::PushID((int)i);
			const std::string title = "[" + std::to_string(i) + "] " +
				(s.surfaceName.empty() ? EntityLabel(scene, s.materialID) : s.surfaceName);
			if (ImGui::TreeNode(title.c_str()))
			{
				EditString("Surface name", s.surfaceName);
				track("Rename Subset");
				EntityRef(scene, "Material", s.materialID);
				ImGui::Text("Indices: %u .. %u", s.indexOffset, s.indexOffset + s.indexCount);
				ImGui::Text("Double sided: %s", s.IsDoubleSided() ? "yes" : "no");
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		ImGui::TreePop();
	}

	if (!m->morph_targets.empty() && ImGui::TreeNode("Morph targets"))
	{
		for (size_t i = 0; i < m->morph_targets.size(); ++i)
		{
			ImGui::PushID((int)i);
			char label[32];
			std::snprintf(label, sizeof(label), "Weight %zu", i);
			ImGui::DragFloat(label, &m->morph_targets[i].weight, 0.005f, 0.0f, 1.0f);
			track("Set Morph Weight");
			ImGui::PopID();
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Geometry operations"))
	{
		ImGui::TextDisabled("These rewrite the vertex buffers. Undo restores them.");
		auto op = [&](const char* label, auto fn) {
			if (ImGui::Button(label))
			{
				if (history) history->BeginEntity(scene, e, label);
				fn();
				if (history) history->Commit(scene);
			}
		};
		op("Flip culling",   [&] { m->FlipCulling(); });
		ImGui::SameLine();
		op("Flip normals",   [&] { m->FlipNormals(); });
		op("Normals: hard",  [&] { m->ComputeNormals(MeshComponent::COMPUTE_NORMALS_HARD); });
		ImGui::SameLine();
		op("smooth",         [&] { m->ComputeNormals(MeshComponent::COMPUTE_NORMALS_SMOOTH); });
		ImGui::SameLine();
		op("smooth (fast)",  [&] { m->ComputeNormals(MeshComponent::COMPUTE_NORMALS_SMOOTH_FAST); });
		op("Recenter",       [&] { m->Recenter(); });
		ImGui::SameLine();
		op("to top",         [&] { m->RecenterToTop(); });
		ImGui::SameLine();
		op("to bottom",      [&] { m->RecenterToBottom(); });
		ImGui::TreePop();
	}
}

// impostor

void DrawImpostor(Scene& scene, Entity e, st::EditorHistory* history)
{
	ImpostorComponent* c = scene.impostors.GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::impostors", "Impostor", false, history)) return;
	const Edits track{ scene, e, history };

	if (ImGui::DragFloat("Swap-in distance", &c->swapInDistance, 0.5f, 0.0f, 100000.0f))
		c->SetDirty();
	track("Set Impostor Distance");
	HelpMarker("Beyond this distance the object is drawn as a captured billboard instead of "
		"its mesh.");

	ImGui::Text("Atlas slot: %d", c->textureIndex);
	if (ImGui::Button("Re-capture"))
	{
		if (history) history->BeginEntity(scene, e, "Recapture Impostor");
		c->SetDirty();
		if (history) history->Commit(scene);
	}
}

// object

void DrawObject(Scene& scene, Entity e, st::EditorHistory* history)
{
	ObjectComponent* o = scene.objects.GetComponent(e);
	if (!o) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::objects", "Object", true, history)) return;
	const Edits track{ scene, e, history };

	EntityRef(scene, "Mesh", o->meshID);

	ImGui::ColorEdit4("Color", &o->color.x);
	track("Set Object Color");
	HelpMarker("Alpha is transparency: the object stops being drawn at all above 0.99.");
	ImGui::ColorEdit4("Emissive", &o->emissiveColor.x);
	track("Set Object Emissive");
	ImGui::DragFloat("Alpha ref", &o->alphaRef, 0.005f, 0.0f, 1.0f);
	track("Set Object Alpha Ref");

	ImGui::DragFloat("LOD bias", &o->lod_bias, 0.01f);
	track("Set LOD Bias");
	ImGui::DragFloat("Draw distance", &o->draw_distance, 1.0f, 0.0f, 1000000.0f);
	track("Set Draw Distance");
	DragUint("Sort priority", o->sort_priority);
	track("Set Sort Priority");
	HelpMarker("Higher draws earlier. Four bits are used.");

	EditMask("Cascade mask", o->cascadeMask);
	track("Set Cascade Mask");
	HelpMarker("Shadow cascades to SKIP, lowest detail first. 0 casts into all of them.");
	EditMask("Filter mask", o->filterMask);
	track("Set Object Filter Mask");

	int userRef = (int)o->userStencilRef;
	if (ImGui::SliderInt("User stencil ref", &userRef, 0, 15))
		o->SetUserStencilRef((uint8_t)userRef);
	track("Set Object Stencil Ref");
	HelpMarker("Above 0 this overrides the material's own user stencil ref.");

	if (ImGui::TreeNodeEx("Flags", ImGuiTreeNodeFlags_DefaultOpen))
	{
		FlagCheckbox("Renderable", [&] { return (o->_flags & ObjectComponent::RENDERABLE) != 0; },
			[&](bool v) { o->SetRenderable(v); });
		FlagCheckbox("Cast shadow", [&] { return o->IsCastingShadow(); },
			[&](bool v) { o->SetCastShadow(v); });
		FlagCheckbox("Dynamic", [&] { return o->IsDynamic(); }, [&](bool v) { o->SetDynamic(v); });
		FlagCheckbox("Request planar reflection", [&] { return o->IsRequestPlanarReflection(); },
			[&](bool v) { o->SetRequestPlanarReflection(v); });
		FlagCheckbox("Foreground", [&] { return o->IsForeground(); },
			[&](bool v) { o->SetForeground(v); });
		HelpMarker("Drawn in front of every regular object, with its own depth range.");
		FlagCheckbox("Hide in main camera", [&] { return o->IsNotVisibleInMainCamera(); },
			[&](bool v) { o->SetNotVisibleInMainCamera(v); });
		FlagCheckbox("Hide in reflections", [&] { return o->IsNotVisibleInReflections(); },
			[&](bool v) { o->SetNotVisibleInReflections(v); });
		FlagCheckbox("Wetmap enabled", [&] { return o->IsWetmapEnabled(); },
			[&](bool v) { o->SetWetmapEnabled(v); });
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Rim highlight"))
	{
		ImGui::ColorEdit4("Rim color", &o->rimHighlightColor.x);
		track("Set Rim Color");
		HelpMarker("Alpha is the amount; 0 turns the highlight off.");
		ImGui::DragFloat("Rim falloff", &o->rimHighlightFalloff, 0.05f, 0.0f, 64.0f);
		track("Set Rim Falloff");
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Lightmap"))
	{
		int size[2] = { (int)o->lightmapWidth, (int)o->lightmapHeight };
		if (ImGui::InputInt2("Resolution", size))
		{
			o->lightmapWidth  = (uint32_t)(size[0] < 0 ? 0 : size[0]);
			o->lightmapHeight = (uint32_t)(size[1] < 0 ? 0 : size[1]);
		}
		track("Set Lightmap Resolution");
		ImGui::Text("Baked data: %zu bytes", o->lightmapTextureData.size());
		ImGui::Text("Iterations: %u", o->lightmapIterationCount);

		FlagCheckbox("Render request", [&] { return o->IsLightmapRenderRequested(); },
			[&](bool v) { o->SetLightmapRenderRequest(v); });
		FlagCheckbox("Disable block compression", [&] { return o->IsLightmapDisableBlockCompression(); },
			[&](bool v) { o->SetLightmapDisableBlockCompression(v); });

		if (ImGui::Button("Clear"))
		{
			if (history) history->BeginEntity(scene, e, "Clear Lightmap");
			o->ClearLightmap();
			if (history) history->Commit(scene);
		}
		ImGui::SameLine();
		if (ImGui::Button("Save from GPU")) o->SaveLightmap();
		ImGui::SameLine();
		if (ImGui::Button("Compress")) o->CompressLightmap();
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Runtime"))
	{
		ImGui::Text("Center: %.2f %.2f %.2f", o->center.x, o->center.y, o->center.z);
		ImGui::Text("Radius: %.3f", o->radius);
		ImGui::Text("LOD: %u", (unsigned)o->lod);
		ImGui::Text("Transparency: %.3f", o->GetTransparency());
		ImGui::Text("Effective filter mask: %08X", o->GetFilterMask());
		ImGui::Text("Vertex AO: %zu bytes", o->vertex_ao.size());
		ImGui::TreePop();
	}
}

// rigid body physics

void DrawRigidBody(Scene& scene, Entity e, st::EditorHistory* history)
{
	RigidBodyPhysicsComponent* c = scene.rigidbodies.GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::rigidbodies", "RigidBody Physics", false, history)) return;
	const Edits track{ scene, e, history };

	// Shape and the per-shape parameters change the BODY, not just its settings, so the
	//	physics object has to be dropped for the system to build a new one next step.
	//	Everything else is a live settings refresh, which is far cheaper.
	bool rebuild = false;
	bool refresh = false;

	static const char* shapes[] = { "Box", "Sphere", "Capsule", "Convex hull",
		"Triangle mesh", "Cylinder", "Heightfield" };
	int shape = (int)c->shape;
	if (ImGui::Combo("Shape", &shape, shapes, IM_ARRAYSIZE(shapes)))
	{
		c->shape = (RigidBodyPhysicsComponent::CollisionShape)shape;
		rebuild = true;
	}
	track("Set Collision Shape");

	switch (c->shape)
	{
	case RigidBodyPhysicsComponent::BOX:
		rebuild |= ImGui::DragFloat3("Half extents", &c->box.halfextents.x, 0.01f, 0.0f, 10000.0f);
		track("Set Box Extents");
		break;
	case RigidBodyPhysicsComponent::SPHERE:
		rebuild |= ImGui::DragFloat("Radius", &c->sphere.radius, 0.01f, 0.0f, 10000.0f);
		track("Set Sphere Radius");
		break;
	case RigidBodyPhysicsComponent::CAPSULE:
	case RigidBodyPhysicsComponent::CYLINDER:
		rebuild |= ImGui::DragFloat("Radius", &c->capsule.radius, 0.01f, 0.0f, 10000.0f);
		track("Set Capsule Radius");
		rebuild |= ImGui::DragFloat("Height", &c->capsule.height, 0.01f, 0.0f, 10000.0f);
		track("Set Capsule Height");
		break;
	case RigidBodyPhysicsComponent::TRIANGLE_MESH:
		rebuild |= DragUint("Mesh LOD", c->mesh_lod);
		track("Set Physics Mesh LOD");
		HelpMarker("Which LOD of the MeshComponent supplies the collision geometry.");
		break;
	default:
		ImGui::TextDisabled("Geometry is taken from the mesh on this entity.");
		break;
	}

	refresh |= ImGui::DragFloat("Mass", &c->mass, 0.05f, 0.0f, 100000.0f);
	track("Set Mass");
	HelpMarker("0 makes the body static.");
	refresh |= ImGui::DragFloat("Friction", &c->friction, 0.005f, 0.0f, 2.0f);
	track("Set Friction");
	refresh |= ImGui::DragFloat("Restitution", &c->restitution, 0.005f, 0.0f, 1.0f);
	track("Set Restitution");
	refresh |= ImGui::DragFloat("Linear damping", &c->damping_linear, 0.005f, 0.0f, 1.0f);
	track("Set Linear Damping");
	refresh |= ImGui::DragFloat("Angular damping", &c->damping_angular, 0.005f, 0.0f, 1.0f);
	track("Set Angular Damping");
	refresh |= ImGui::DragFloat("Buoyancy", &c->buoyancy, 0.01f, 0.0f, 10.0f);
	track("Set Buoyancy");
	rebuild |= ImGui::DragFloat3("Local offset", &c->local_offset.x, 0.01f);
	track("Set Physics Offset");

	FlagCheckbox("Kinematic", [&] { return c->IsKinematic(); },
		[&](bool v) { c->SetKinematic(v); refresh = true; });
	HelpMarker("Driven by its transform rather than by forces, and it pushes other bodies.");
	FlagCheckbox("Disable deactivation", [&] { return c->IsDisableDeactivation(); },
		[&](bool v) { c->SetDisableDeactivation(v); refresh = true; });
	FlagCheckbox("Start deactivated", [&] { return c->IsStartDeactivated(); },
		[&](bool v) { c->SetStartDeactivated(v); refresh = true; });
	FlagCheckbox("Character physics", [&] { return c->IsCharacterPhysics(); },
		[&](bool v) { c->SetCharacterPhysics(v); rebuild = true; });

	if (c->IsCharacterPhysics() && ImGui::TreeNodeEx("Character", ImGuiTreeNodeFlags_DefaultOpen))
	{
		refresh |= DragAngle("Max slope angle", c->character.maxSlopeAngle, 0.25f, 0.0f, 89.0f);
		track("Set Max Slope Angle");
		refresh |= ImGui::DragFloat("Gravity factor", &c->character.gravityFactor, 0.01f, -10.0f, 10.0f);
		track("Set Gravity Factor");
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Vehicle"))
	{
		static const char* vehicleTypes[] = { "None", "Car", "Motorcycle" };
		int vt = (int)c->vehicle.type;
		if (ImGui::Combo("Type", &vt, vehicleTypes, IM_ARRAYSIZE(vehicleTypes)))
		{
			c->vehicle.type = (RigidBodyPhysicsComponent::Vehicle::Type)vt;
			rebuild = true;
		}
		track("Set Vehicle Type");

		if (c->IsVehicle())
		{
			static const char* modes[] = { "Ray", "Sphere", "Cylinder" };
			int cm = (int)c->vehicle.collision_mode;
			if (ImGui::Combo("Wheel collision", &cm, modes, IM_ARRAYSIZE(modes)))
			{
				c->vehicle.collision_mode = (RigidBodyPhysicsComponent::Vehicle::CollisionMode)cm;
				rebuild = true;
			}
			track("Set Wheel Collision Mode");

			rebuild |= ImGui::DragFloat("Chassis half width", &c->vehicle.chassis_half_width, 0.01f);
			track("Set Chassis Width");
			rebuild |= ImGui::DragFloat("Chassis half height", &c->vehicle.chassis_half_height, 0.01f);
			track("Set Chassis Height");
			rebuild |= ImGui::DragFloat("Chassis half length", &c->vehicle.chassis_half_length, 0.01f);
			track("Set Chassis Length");
			rebuild |= ImGui::DragFloat("Front wheel offset", &c->vehicle.front_wheel_offset, 0.01f);
			track("Set Front Wheel Offset");
			rebuild |= ImGui::DragFloat("Rear wheel offset", &c->vehicle.rear_wheel_offset, 0.01f);
			track("Set Rear Wheel Offset");
			rebuild |= ImGui::DragFloat("Wheel radius", &c->vehicle.wheel_radius, 0.005f, 0.0f, 10.0f);
			track("Set Wheel Radius");
			rebuild |= ImGui::DragFloat("Wheel width", &c->vehicle.wheel_width, 0.005f, 0.0f, 10.0f);
			track("Set Wheel Width");
			refresh |= ImGui::DragFloat("Max engine torque", &c->vehicle.max_engine_torque, 1.0f, 0.0f, 100000.0f);
			track("Set Engine Torque");
			refresh |= ImGui::DragFloat("Clutch strength", &c->vehicle.clutch_strength, 0.1f, 0.0f, 1000.0f);
			track("Set Clutch Strength");
			refresh |= DragAngle("Max roll angle", c->vehicle.max_roll_angle, 0.25f, 0.0f, 90.0f);
			track("Set Max Roll Angle");
			refresh |= DragAngle("Max steering angle", c->vehicle.max_steering_angle, 0.25f, 0.0f, 90.0f);
			track("Set Max Steering Angle");

			auto suspension = [&](const char* label, RigidBodyPhysicsComponent::Vehicle::Suspension& s) {
				if (!ImGui::TreeNode(label)) return;
				refresh |= ImGui::DragFloat("Min length", &s.min_length, 0.005f, 0.0f, 10.0f);
				track("Set Suspension Min Length");
				refresh |= ImGui::DragFloat("Max length", &s.max_length, 0.005f, 0.0f, 10.0f);
				track("Set Suspension Max Length");
				refresh |= ImGui::DragFloat("Frequency", &s.frequency, 0.01f, 0.0f, 100.0f);
				track("Set Suspension Frequency");
				refresh |= ImGui::DragFloat("Damping", &s.damping, 0.005f, 0.0f, 10.0f);
				track("Set Suspension Damping");
				ImGui::TreePop();
			};
			suspension("Front suspension", c->vehicle.front_suspension);
			suspension("Rear suspension",  c->vehicle.rear_suspension);

			if (c->IsCar())
			{
				if (ImGui::Checkbox("Four wheel drive", &c->vehicle.car.four_wheel_drive))
					refresh = true;
				track("Set Four Wheel Drive");
			}
			if (c->IsMotorcycle())
			{
				refresh |= DragAngle("Front suspension angle",
					c->vehicle.motorcycle.front_suspension_angle, 0.25f, 0.0f, 90.0f);
				track("Set Front Suspension Angle");
				refresh |= ImGui::DragFloat("Front brake torque",
					&c->vehicle.motorcycle.front_brake_torque, 1.0f, 0.0f, 100000.0f);
				track("Set Front Brake Torque");
				refresh |= ImGui::DragFloat("Rear brake torque",
					&c->vehicle.motorcycle.rear_brake_torque, 1.0f, 0.0f, 100000.0f);
				track("Set Rear Brake Torque");
				ImGui::Checkbox("Lean control", &c->vehicle.motorcycle.lean_control);
				HelpMarker("Not serialized: a runtime assist that keeps the bike upright.");
			}

			EntityRef(scene, "Wheel: front left",  c->vehicle.wheel_entity_front_left);
			EntityRef(scene, "Wheel: front right", c->vehicle.wheel_entity_front_right);
			EntityRef(scene, "Wheel: rear left",   c->vehicle.wheel_entity_rear_left);
			EntityRef(scene, "Wheel: rear right",  c->vehicle.wheel_entity_rear_right);
		}
		ImGui::TreePop();
	}

	ImGui::TextDisabled(c->physicsobject ? "body: live" : "body: not created");
	if (ImGui::Button("Rebuild body")) rebuild = true;

	if (rebuild)      c->physicsobject.reset();   // rebuilt on the next physics step
	else if (refresh) c->SetRefreshParametersNeeded();
}

// physics constraint

void DrawConstraint(Scene& scene, Entity e, st::EditorHistory* history)
{
	PhysicsConstraintComponent* c = scene.constraints.GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::constraints", "Physics Constraint", false, history)) return;
	const Edits track{ scene, e, history };

	bool rebuild = false;
	bool refresh = false;

	static const char* types[] = { "Fixed", "Point", "Distance", "Hinge", "Cone",
		"Six DOF", "Swing/twist", "Slider" };
	int type = (int)c->type;
	if (ImGui::Combo("Type", &type, types, IM_ARRAYSIZE(types)))
	{
		c->type = (PhysicsConstraintComponent::Type)type;
		rebuild = true;
	}
	track("Set Constraint Type");
	HelpMarker("The constraint axes come from the transform on this entity: RIGHT is X, UP is Y.");

	EntityRef(scene, "Body A", c->bodyA);
	EntityRef(scene, "Body B", c->bodyB);

	switch (c->type)
	{
	case PhysicsConstraintComponent::Type::Distance:
		refresh |= ImGui::DragFloat("Min distance", &c->distance_constraint.min_distance, 0.01f);
		track("Set Min Distance");
		refresh |= ImGui::DragFloat("Max distance", &c->distance_constraint.max_distance, 0.01f);
		track("Set Max Distance");
		break;
	case PhysicsConstraintComponent::Type::Hinge:
		refresh |= DragAngle("Min angle", c->hinge_constraint.min_angle, 0.25f, -180.0f, 180.0f);
		track("Set Hinge Min Angle");
		refresh |= DragAngle("Max angle", c->hinge_constraint.max_angle, 0.25f, -180.0f, 180.0f);
		track("Set Hinge Max Angle");
		refresh |= ImGui::DragFloat("Motor target velocity", &c->hinge_constraint.target_angular_velocity, 0.01f);
		track("Set Hinge Motor Velocity");
		break;
	case PhysicsConstraintComponent::Type::Cone:
		refresh |= DragAngle("Half cone angle", c->cone_constraint.half_cone_angle, 0.25f, 0.0f, 180.0f);
		track("Set Cone Angle");
		break;
	case PhysicsConstraintComponent::Type::SixDOF:
		refresh |= ImGui::DragFloat3("Min translation", &c->six_dof.minTranslationAxes.x, 0.01f);
		track("Set Min Translation");
		refresh |= ImGui::DragFloat3("Max translation", &c->six_dof.maxTranslationAxes.x, 0.01f);
		track("Set Max Translation");
		refresh |= ImGui::DragFloat3("Min rotation (rad)", &c->six_dof.minRotationAxes.x, 0.01f);
		track("Set Min Rotation");
		refresh |= ImGui::DragFloat3("Max rotation (rad)", &c->six_dof.maxRotationAxes.x, 0.01f);
		track("Set Max Rotation");
		HelpMarker("min greater than max on an axis means FIXED, and the full range means free. "
			"The SetFixed / SetFree helpers write exactly those sentinel values.");
		break;
	case PhysicsConstraintComponent::Type::SwingTwist:
		refresh |= DragAngle("Normal half cone", c->swing_twist.normal_half_cone_angle, 0.25f, 0.0f, 180.0f);
		track("Set Normal Half Cone");
		refresh |= DragAngle("Plane half cone", c->swing_twist.plane_half_cone_angle, 0.25f, 0.0f, 180.0f);
		track("Set Plane Half Cone");
		refresh |= DragAngle("Min twist", c->swing_twist.min_twist_angle, 0.25f, -180.0f, 180.0f);
		track("Set Min Twist");
		refresh |= DragAngle("Max twist", c->swing_twist.max_twist_angle, 0.25f, -180.0f, 180.0f);
		track("Set Max Twist");
		break;
	case PhysicsConstraintComponent::Type::Slider:
		refresh |= ImGui::DragFloat("Min limit", &c->slider_constraint.min_limit, 0.01f);
		track("Set Slider Min");
		refresh |= ImGui::DragFloat("Max limit", &c->slider_constraint.max_limit, 0.01f);
		track("Set Slider Max");
		refresh |= ImGui::DragFloat("Motor target velocity", &c->slider_constraint.target_velocity, 0.01f);
		track("Set Slider Motor Velocity");
		refresh |= ImGui::DragFloat("Motor max force (N)", &c->slider_constraint.max_force, 1.0f, 0.0f, 1000000.0f);
		track("Set Slider Motor Force");
		break;
	default:
		ImGui::TextDisabled("This type has no extra settings.");
		break;
	}

	refresh |= ImGui::DragFloat("Break distance", &c->break_distance, 0.01f, 0.0f, FLT_MAX);
	track("Set Break Distance");
	HelpMarker("Relative distance the constraint may be stretched before it breaks.");

	FlagCheckbox("Disable self collision", [&] { return c->IsDisableSelfCollision(); },
		[&](bool v) { c->SetDisableSelfCollision(v); rebuild = true; });

	ImGui::TextDisabled(c->physicsobject ? "constraint: live" : "constraint: not created");
	if (ImGui::Button("Rebuild constraint")) rebuild = true;

	if (rebuild)      c->physicsobject.reset();
	else if (refresh) c->SetRefreshParametersNeeded();
}

// soft body physics

void DrawSoftBody(Scene& scene, Entity e, st::EditorHistory* history)
{
	SoftBodyPhysicsComponent* c = scene.softbodies.GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::softbodies", "SoftBody Physics", false, history)) return;
	const Edits track{ scene, e, history };

	bool rebuild = false;

	rebuild |= ImGui::DragFloat("Mass", &c->mass, 0.05f, 0.0f, 100000.0f);
	track("Set Soft Body Mass");
	rebuild |= ImGui::DragFloat("Friction", &c->friction, 0.005f, 0.0f, 2.0f);
	track("Set Soft Body Friction");
	rebuild |= ImGui::DragFloat("Restitution", &c->restitution, 0.005f, 0.0f, 1.0f);
	track("Set Soft Body Restitution");
	rebuild |= ImGui::DragFloat("Pressure", &c->pressure, 0.01f, 0.0f, 1000.0f);
	track("Set Soft Body Pressure");
	rebuild |= ImGui::DragFloat("Vertex radius", &c->vertex_radius, 0.005f, 0.0f, 10.0f);
	track("Set Vertex Radius");
	HelpMarker("How far the simulated vertices keep away from other physics bodies.");

	float detail = c->detail;
	if (ImGui::DragFloat("Detail", &detail, 0.005f, 0.0f, 1.0f))
		c->SetDetail(detail);   // also resets the body, since the physics mesh changes
	track("Set Soft Body Detail");

	FlagCheckbox("Disable deactivation", [&] { return c->IsDisableDeactivation(); },
		[&](bool v) { c->SetDisableDeactivation(v); });
	FlagCheckbox("Wind enabled", [&] { return c->IsWindEnabled(); },
		[&](bool v) { c->SetWindEnabled(v); });

	ImGui::Text("Physics vertices: %zu", c->physicsToGraphicsVertexMapping.size());
	ImGui::Text("Weights: %zu", c->weights.size());
	ImGui::TextDisabled(c->physicsobject ? "body: live" : "body: not created");

	if (ImGui::Button("Rebuild from mesh"))
	{
		if (history) history->BeginEntity(scene, e, "Rebuild Soft Body");
		c->Reset();
		if (history) history->Commit(scene);
	}
	if (rebuild) c->physicsobject.reset();
}

// armature

void DrawArmature(Scene& scene, Entity e, st::EditorHistory* history)
{
	ArmatureComponent* c = scene.armatures.GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::armatures", "Armature", false, history)) return;

	ImGui::Text("%zu bones", c->boneCollection.size());
	ImGui::TextDisabled("The bone list and its inverse bind matrices come from the import; "
		"pose a bone by selecting it and editing its Transform.");

	if (!c->boneCollection.empty() && ImGui::TreeNode("Bones"))
	{
		for (size_t i = 0; i < c->boneCollection.size(); ++i)
			ImGui::BulletText("%zu: %s", i, EntityLabel(scene, c->boneCollection[i]).c_str());
		ImGui::TreePop();
	}

	ImGui::Text("AABB: %.2f %.2f %.2f .. %.2f %.2f %.2f",
		c->aabb._min.x, c->aabb._min.y, c->aabb._min.z,
		c->aabb._max.x, c->aabb._max.y, c->aabb._max.z);
}

// light

void DrawLight(Scene& scene, Entity e, st::EditorHistory* history)
{
	LightComponent* l = scene.lights.GetComponent(e);
	if (!l) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::lights", "Light", true, history)) return;
	const Edits track{ scene, e, history };

	static const char* types[] = { "Directional", "Point", "Spot", "Rectangle" };
	int ty = (int)l->type;
	if (ImGui::Combo("Type", &ty, types, IM_ARRAYSIZE(types)))
		l->type = (LightComponent::LightType)ty;
	track("Set Light Type");

	ImGui::ColorEdit3("Color", &l->color.x);
	track("Set Light Color");
	ImGui::DragFloat("Intensity", &l->intensity, 0.5f, 0.0f, 100000.0f);
	track("Set Light Intensity");
	HelpMarker("Point and spot lights are luminous intensity in candela; a directional light "
		"is illuminance in lux.");
	ImGui::DragFloat("Range", &l->range, 0.1f, 0.0f, 100000.0f);
	track("Set Light Range");
	ImGui::DragFloat("Radius", &l->radius, 0.001f, 0.0f, 1000.0f);
	track("Set Light Radius");
	HelpMarker("Source size. Larger radius means softer shadows and a visible sphere/tube.");

	if (l->type == LightComponent::POINT || l->type == LightComponent::RECTANGLE)
	{
		ImGui::DragFloat("Length", &l->length, 0.01f, 0.0f, 1000.0f);
		track("Set Light Length");
		HelpMarker("Point light: tube length. Rectangle light: width.");
	}
	if (l->type == LightComponent::RECTANGLE)
	{
		ImGui::DragFloat("Height", &l->height, 0.01f, 0.0f, 1000.0f);
		track("Set Light Height");
	}
	if (l->type == LightComponent::SPOT)
	{
		DragAngle("Outer cone", l->outerConeAngle, 0.25f, 0.0f, 90.0f);
		track("Set Outer Cone");
		DragAngle("Inner cone", l->innerConeAngle, 0.25f, 0.0f, 90.0f);
		track("Set Inner Cone");
		HelpMarker("Inner cone 0 means the falloff uses the outer cone alone.");
	}

	if (ImGui::TreeNodeEx("Flags", ImGuiTreeNodeFlags_DefaultOpen))
	{
		FlagCheckbox("Cast shadow", [&] { return l->IsCastingShadow(); },
			[&](bool v) { l->SetCastShadow(v); });
		FlagCheckbox("Volumetrics", [&] { return l->IsVolumetricsEnabled(); },
			[&](bool v) { l->SetVolumetricsEnabled(v); });
		FlagCheckbox("Visualizer", [&] { return l->IsVisualizerEnabled(); },
			[&](bool v) { l->SetVisualizerEnabled(v); });
		HelpMarker("Draws the emissive shape of the light itself.");
		FlagCheckbox("Static (lightmap only)", [&] { return l->IsStatic(); },
			[&](bool v) { l->SetStatic(v); });
		FlagCheckbox("Lights volumetric clouds", [&] { return l->IsVolumetricCloudsEnabled(); },
			[&](bool v) { l->SetVolumetricCloudsEnabled(v); });
		ImGui::TreePop();
	}

	ImGui::DragFloat("Volumetric boost", &l->volumetric_boost, 0.01f, 0.0f, 100.0f);
	track("Set Volumetric Boost");

	if (ImGui::TreeNode("Shadows"))
	{
		int forced = l->forced_shadow_resolution;
		if (ImGui::InputInt("Forced resolution", &forced))
			l->forced_shadow_resolution = forced;
		track("Set Forced Shadow Resolution");
		HelpMarker("-1 lets the shadow atlas pick a size from screen coverage.");

		if (l->type == LightComponent::DIRECTIONAL)
		{
			ImGui::Text("Cascades: %zu", l->cascade_distances.size());
			for (size_t i = 0; i < l->cascade_distances.size(); ++i)
			{
				ImGui::PushID((int)i);
				char label[32];
				std::snprintf(label, sizeof(label), "Cascade %zu", i);
				ImGui::DragFloat(label, &l->cascade_distances[i], 0.5f, 0.0f, 100000.0f);
				track("Set Cascade Distance");
				ImGui::PopID();
			}
			if (ImGui::Button("Add cascade"))
			{
				if (history) history->BeginEntity(scene, e, "Add Cascade");
				l->cascade_distances.push_back(l->cascade_distances.empty()
					? 8.0f : l->cascade_distances.back() * 4.0f);
				if (history) history->Commit(scene);
			}
			ImGui::SameLine();
			if (ImGui::Button("Remove last") && !l->cascade_distances.empty())
			{
				if (history) history->BeginEntity(scene, e, "Remove Cascade");
				l->cascade_distances.pop_back();
				if (history) history->Commit(scene);
			}
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Lens flare"))
	{
		for (size_t i = 0; i < l->lensFlareNames.size(); ++i)
		{
			ImGui::PushID((int)i);
			char label[32];
			std::snprintf(label, sizeof(label), "Rim %zu", i);
			EditString(label, l->lensFlareNames[i]);
			track("Set Lens Flare Texture");
			ImGui::PopID();
		}
		if (ImGui::Button("Add rim"))
		{
			if (history) history->BeginEntity(scene, e, "Add Lens Flare Rim");
			l->lensFlareNames.emplace_back();
			if (history) history->Commit(scene);
		}
		ImGui::SameLine();
		if (ImGui::Button("Remove last") && !l->lensFlareNames.empty())
		{
			if (history) history->BeginEntity(scene, e, "Remove Lens Flare Rim");
			l->lensFlareNames.pop_back();
			if (history) history->Commit(scene);
		}
		ImGui::TextDisabled("Engine lens flare. The framework StLensFlare is a separate, "
			"procedural effect and does not read this list.");
		ImGui::TreePop();
	}

	EntityRef(scene, "Camera source", l->cameraSource);
	HelpMarker("Projects that camera render as the light mask, which is how a projector light "
			"gets its image.");

	if (ImGui::TreeNode("Runtime"))
	{
		ImGui::Text("Position: %.2f %.2f %.2f", l->position.x, l->position.y, l->position.z);
		ImGui::Text("Direction: %.3f %.3f %.3f", l->direction.x, l->direction.y, l->direction.z);
		ImGui::Text("Inactive: %s", l->IsInactive() ? "yes" : "no");
		ImGui::TreePop();
	}
}

// camera

void DrawCamera(Scene& scene, Entity e, st::EditorHistory* history)
{
	CameraComponent* c = scene.cameras.GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::cameras", "Camera", false, history)) return;
	const Edits track{ scene, e, history };

	bool dirty = false;

	bool ortho = c->IsOrtho();
	if (ImGui::Checkbox("Orthographic", &ortho)) c->SetOrtho(ortho);
	if (ortho)
	{
		dirty |= ImGui::DragFloat("Ortho vertical size", &c->ortho_vertical_size, 0.05f, 0.001f, 100000.0f);
		track("Set Ortho Size");
	}
	else
	{
		dirty |= DragAngle("FOV", c->fov, 0.25f, 1.0f, 179.0f);
		track("Set FOV");
	}

	dirty |= ImGui::DragFloat("Near plane", &c->zNearP, 0.01f, 0.001f, 100000.0f);
	track("Set Near Plane");
	dirty |= ImGui::DragFloat("Far plane", &c->zFarP, 1.0f, 0.01f, 1000000.0f);
	track("Set Far Plane");

	ImGui::DragFloat("Focal length", &c->focal_length, 0.01f, 0.0f, 10000.0f);
	track("Set Focal Length");
	ImGui::DragFloat("Aperture size", &c->aperture_size, 0.01f, 0.0f, 100.0f);
	track("Set Aperture Size");
	HelpMarker("0 disables depth of field.");
	ImGui::DragFloat2("Aperture shape", &c->aperture_shape.x, 0.01f, 0.0f, 10.0f);
	track("Set Aperture Shape");

	FlagCheckbox("Custom projection", [&] { return c->IsCustomProjectionEnabled(); },
		[&](bool v) { c->SetCustomProjectionEnabled(v); });
	HelpMarker("Stops UpdateCamera from rebuilding the projection matrix, so code that wrote "
		"one by hand keeps it.");
	FlagCheckbox("CRT filter", [&] { return c->IsCRT(); }, [&](bool v) { c->SetCRT(v); });

	// Large-world absolute position: the render origin is normally set from this every frame,
	//	so it is the camera field that actually places the camera in the world.
	double wp[3] = { c->GetWorldPositionX(), c->GetWorldPositionY(), c->GetWorldPositionZ() };
	if (ImGui::DragScalarN("World pos (abs)", ImGuiDataType_Double, wp, 3, 0.1f))
		c->SetWorldPosition(wp[0], wp[1], wp[2]);
	track("Set Camera World Position");

	if (ImGui::TreeNode("Render to texture"))
	{
		int res[2] = { (int)c->render_to_texture.resolution.x, (int)c->render_to_texture.resolution.y };
		if (ImGui::InputInt2("Resolution", res))
		{
			c->render_to_texture.resolution.x = (uint32_t)(res[0] < 0 ? 0 : res[0]);
			c->render_to_texture.resolution.y = (uint32_t)(res[1] < 0 ? 0 : res[1]);
		}
		track("Set Render Target Resolution");
		HelpMarker("Non-zero turns this camera into an off-screen render, which a material or "
			"a light mask can then sample.");

		int samples = (int)c->render_to_texture.sample_count;
		if (ImGui::SliderInt("MSAA samples", &samples, 1, 8))
			c->render_to_texture.sample_count = (uint32_t)samples;
		track("Set Render Target MSAA");

		ImGui::DragFloat("Update interval (s)", &c->render_to_texture.update_interval, 0.01f, 0.0f, 60.0f);
		track("Set Render Target Interval");
		HelpMarker("0 renders every frame.");
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Runtime"))
	{
		ImGui::Text("Eye: %.2f %.2f %.2f", c->Eye.x, c->Eye.y, c->Eye.z);
		ImGui::Text("At:  %.3f %.3f %.3f", c->At.x, c->At.y, c->At.z);
		ImGui::Text("Up:  %.3f %.3f %.3f", c->Up.x, c->Up.y, c->Up.z);
		ImGui::Text("Viewport: %.0f x %.0f", c->width, c->height);
		ImGui::Text("Sample count: %u", c->sample_count);
		ImGui::TreePop();
	}

	if (dirty)
		c->SetDirty();
}

// environment probe

void DrawProbe(Scene& scene, Entity e, st::EditorHistory* history)
{
	EnvironmentProbeComponent* c = scene.probes.GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::probes", "Environment Probe", false, history)) return;
	const Edits track{ scene, e, history };

	// Resolution and the texture source change what has to be allocated, so both go through
	//	SetDirty, which also drops the current cubemap.
	int res = (int)c->resolution;
	if (ImGui::InputInt("Resolution", &res))
	{
		// Snap to the nearest power of two at or below the typed value: the cubemap array is
		// allocated on that assumption and a stray 300 would fail to create.
		uint32_t v = (uint32_t)(res < 8 ? 8 : res);
		uint32_t pow2 = 8;
		while (pow2 * 2 <= v && pow2 < 4096) pow2 *= 2;
		c->resolution = pow2;
		c->SetDirty();
	}
	track("Set Probe Resolution");

	FlagCheckbox("Realtime", [&] { return c->IsRealTime(); }, [&](bool v) { c->SetRealTime(v); });
	if (c->IsRealTime())
	{
		float interval = c->GetRealtimeUpdateInterval();
		if (ImGui::DragFloat("Update interval (s)", &interval, 0.01f, 0.0f, 60.0f))
			c->SetUpdateInterval(interval);
		track("Set Probe Interval");
		HelpMarker("0 re-renders every frame, which is the expensive setting.");
	}
	FlagCheckbox("MSAA", [&] { return c->IsMSAA(); }, [&](bool v) { c->SetMSAA(v); });

	ImGui::DragFloat("View distance", &c->view_distance, 0.5f, -1.0f, 1000000.0f);
	track("Set Probe View Distance");
	HelpMarker("-1 uses the main camera far plane.");

	// On commit, not per keystroke: SetDirty drops the baked cubemap and queues a re-render,
	//	which is not something to do once per character typed.
	AssetDropField("Texture asset", c->textureName);
	if (ImGui::IsItemDeactivatedAfterEdit())
		c->SetDirty();
	track("Set Probe Texture");
	HelpMarker("Set this to bake from a file instead of rendering the probe.");

	ImGui::Text("Memory: %zu bytes", c->GetMemorySizeInBytes());
	ImGui::TextDisabled(c->IsDirty() ? "queued for render" : "up to date");
	if (ImGui::Button("Re-render now"))
	{
		if (history) history->BeginEntity(scene, e, "Refresh Probe");
		c->SetDirty();
		if (history) history->Commit(scene);
	}
}

// force field

void DrawForceField(Scene& scene, Entity e, st::EditorHistory* history)
{
	ForceFieldComponent* c = scene.forces.GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::forces", "Force Field", false, history)) return;
	const Edits track{ scene, e, history };

	static const char* types[] = { "Point", "Plane" };
	int ty = (int)c->type;
	if (ImGui::Combo("Type", &ty, types, IM_ARRAYSIZE(types)))
		c->type = (ForceFieldComponent::Type)ty;
	track("Set Force Field Type");
	HelpMarker("A plane field pushes along the transform UP axis; a point field pushes along "
		"the vector from its origin.");

	ImGui::DragFloat("Gravity", &c->gravity, 0.05f, -1000.0f, 1000.0f);
	track("Set Force Gravity");
	HelpMarker("Negative deflects, positive attracts.");
	ImGui::DragFloat("Range", &c->range, 0.1f, 0.0f, 100000.0f);
	track("Set Force Range");
}

// decal

void DrawDecal(Scene& scene, Entity e, st::EditorHistory* history)
{
	DecalComponent* c = scene.decals.GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::decals", "Decal", false, history)) return;
	const Edits track{ scene, e, history };

	ImGui::DragFloat("Slope blend power", &c->slopeBlendPower, 0.01f, 0.0f, 16.0f);
	track("Set Slope Blend Power");
	HelpMarker("Fades the decal out on surfaces facing away from its projection axis.");

	FlagCheckbox("Base color: alpha only", [&] { return c->IsBaseColorOnlyAlpha(); },
		[&](bool v) { c->SetBaseColorOnlyAlpha(v); });
	HelpMarker("Takes only the alpha channel from the base colour texture, which is what a "
		"normal-map-only decal wants.");

	ImGui::Separator();
	ImGui::TextDisabled("Colour, opacity, emissive and the four textures come from the "
		"MaterialComponent on this same entity; the decal system copies them here each frame.");
	ImGui::Text("Color: %.2f %.2f %.2f  opacity %.2f", c->color.x, c->color.y, c->color.z, c->GetOpacity());
	ImGui::Text("Emissive: %.2f", c->emissive);
	ImGui::Text("Normal strength: %.2f", c->normal_strength);
	ImGui::Text("Displacement strength: %.2f", c->displacement_strength);
	ImGui::Text("Range: %.2f", c->range);
	ImGui::Text("Textures: base %s, normal %s, surface %s, displacement %s",
		c->texture.IsValid() ? "yes" : "no",
		c->normal.IsValid() ? "yes" : "no",
		c->surfacemap.IsValid() ? "yes" : "no",
		c->displacementmap.IsValid() ? "yes" : "no");
}

// animation

const char* AnimationPathName(AnimationComponent::AnimationChannel::Path path)
{
	using Path = AnimationComponent::AnimationChannel::Path;
	switch (path)
	{
	case Path::TRANSLATION:          return "translation";
	case Path::ROTATION:             return "rotation";
	case Path::SCALE:                return "scale";
	case Path::WEIGHTS:              return "morph weights";
	case Path::LIGHT_COLOR:          return "light color";
	case Path::LIGHT_INTENSITY:      return "light intensity";
	case Path::LIGHT_RANGE:          return "light range";
	case Path::LIGHT_INNERCONE:      return "light inner cone";
	case Path::LIGHT_OUTERCONE:      return "light outer cone";
	case Path::SOUND_PLAY:           return "sound play";
	case Path::SOUND_STOP:           return "sound stop";
	case Path::SOUND_VOLUME:         return "sound volume";
	case Path::EMITTER_EMITCOUNT:    return "emitter emit count";
	case Path::CAMERA_FOV:           return "camera fov";
	case Path::CAMERA_FOCAL_LENGTH:  return "camera focal length";
	case Path::CAMERA_APERTURE_SIZE: return "camera aperture size";
	case Path::CAMERA_APERTURE_SHAPE:return "camera aperture shape";
	case Path::SCRIPT_PLAY:          return "script play";
	case Path::SCRIPT_STOP:          return "script stop";
	case Path::MATERIAL_COLOR:       return "material color";
	case Path::MATERIAL_EMISSIVE:    return "material emissive";
	case Path::MATERIAL_ROUGHNESS:   return "material roughness";
	case Path::MATERIAL_METALNESS:   return "material metalness";
	case Path::MATERIAL_REFLECTANCE: return "material reflectance";
	case Path::MATERIAL_TEXMULADD:   return "material tex mul/add";
	default:                         return "unknown";
	}
}

void DrawAnimation(Scene& scene, Entity e, st::EditorHistory* history)
{
	AnimationComponent* a = scene.animations.GetComponent(e);
	if (!a) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::animations", "Animation", false, history)) return;
	const Edits track{ scene, e, history };

	// Transport buttons write the PLAYING flag, which is a one-frame change rather than a
	//	drag, so they bracket their own undo step instead of going through `track`.
	auto transport = [&](const char* label, auto fn) {
		if (!ImGui::Button(label)) return;
		if (history) history->BeginEntity(scene, e, label);
		fn();
		if (history) history->Commit(scene);
	};
	transport(a->IsPlaying() ? "Pause" : "Play", [&] { a->IsPlaying() ? a->Pause() : a->Play(); });
	ImGui::SameLine();
	transport("Stop", [&] { a->Stop(); });
	ImGui::SameLine();
	ImGui::TextDisabled(a->IsPlaying() ? "playing" : "stopped");

	ImGui::DragFloat("Timer", &a->timer, 0.01f, a->start, a->end);
	track("Scrub Animation");
	ImGui::DragFloat("Start", &a->start, 0.01f);
	track("Set Animation Start");
	ImGui::DragFloat("End", &a->end, 0.01f);
	track("Set Animation End");
	ImGui::DragFloat("Blend amount", &a->amount, 0.005f, 0.0f, 1.0f);
	track("Set Animation Amount");
	ImGui::DragFloat("Speed", &a->speed, 0.01f, -10.0f, 10.0f);
	track("Set Animation Speed");
	ImGui::Text("Length: %.3f s", a->GetLength());

	// Looped and ping-pong are mutually exclusive in the setters, so they are drawn as one
	//	three-way choice rather than two checkboxes that silently clear each other.
	int mode = a->IsLooped() ? 1 : (a->IsPingPong() ? 2 : 0);
	static const char* modes[] = { "Play once", "Loop", "Ping-pong" };
	if (ImGui::Combo("Loop mode", &mode, modes, IM_ARRAYSIZE(modes)))
	{
		if (mode == 0) a->SetPlayOnce();
		else if (mode == 1) a->SetLooped();
		else a->SetPingPong();
	}
	track("Set Loop Mode");

	bool rootMotion = a->IsRootMotion();
	if (ImGui::Checkbox("Root motion", &rootMotion))
		rootMotion ? a->RootMotionOn() : a->RootMotionOff();
	track("Set Root Motion");
	if (a->IsRootMotion())
	{
		EntityRef(scene, "Root motion bone", a->rootMotionBone);
		ImGui::Text("Offset: %.3f %.3f %.3f", a->rootTranslationOffset.x,
			a->rootTranslationOffset.y, a->rootTranslationOffset.z);
	}

	if (!a->channels.empty() && ImGui::TreeNode("Channels"))
	{
		for (size_t i = 0; i < a->channels.size(); ++i)
		{
			const AnimationComponent::AnimationChannel& ch = a->channels[i];
			ImGui::BulletText("%zu: %s -> %s (sampler %d)", i,
				EntityLabel(scene, ch.target).c_str(), AnimationPathName(ch.path), ch.samplerIndex);
		}
		ImGui::TreePop();
	}

	if (!a->samplers.empty() && ImGui::TreeNode("Samplers"))
	{
		static const char* sampleModes[] = { "Linear", "Step", "Cubic spline" };
		for (size_t i = 0; i < a->samplers.size(); ++i)
		{
			AnimationComponent::AnimationSampler& s = a->samplers[i];
			ImGui::PushID((int)i);
			char label[32];
			std::snprintf(label, sizeof(label), "Sampler %zu", i);
			int sm = (int)s.mode;
			if (sm >= 0 && sm < IM_ARRAYSIZE(sampleModes) &&
				ImGui::Combo(label, &sm, sampleModes, IM_ARRAYSIZE(sampleModes)))
				s.mode = (AnimationComponent::AnimationSampler::Mode)sm;
			track("Set Sampler Mode");
			ImGui::SameLine();
			ImGui::TextDisabled("data: %s", EntityLabel(scene, s.data).c_str());
			ImGui::PopID();
		}
		ImGui::TreePop();
	}

	if (!a->retargets.empty())
		ImGui::Text("Retarget sources: %zu", a->retargets.size());
}

// animation data

void DrawAnimationData(Scene& scene, Entity e, st::EditorHistory* history)
{
	AnimationDataComponent* c = scene.animation_datas.GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::animation_datas", "Animation Data", false, history)) return;

	ImGui::Text("%zu keyframe times, %zu data floats",
		c->keyframe_times.size(), c->keyframe_data.size());
	if (!c->keyframe_times.empty())
		ImGui::Text("Range: %.3f .. %.3f s", c->keyframe_times.front(), c->keyframe_times.back());
	ImGui::TextDisabled("Raw sampler data shared by AnimationComponents. It is imported, not "
		"authored here; a curve editor is the tool for this and this panel is not it.");
}

// emitted particles

void DrawEmitter(Scene& scene, Entity e, st::EditorHistory* history)
{
	wi::EmittedParticleSystem* c = scene.emitters.GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::emitters", "Emitted Particles", false, history)) return;
	const Edits track{ scene, e, history };

	static const char* shaderTypes[] = { "Soft", "Soft + distortion", "Simple", "Soft + lighting" };
	int st = (int)c->shaderType;
	if (ImGui::Combo("Shader", &st, shaderTypes, IM_ARRAYSIZE(shaderTypes)))
		c->shaderType = (wi::EmittedParticleSystem::PARTICLESHADERTYPE)st;
	track("Set Particle Shader");

	// Applied when the drag ENDS. SetMaxParticleCount reallocates every particle buffer on
	//	the GPU, so running it on each frame of a drag from 1,000 to 100,000 would allocate
	//	and throw away a hundred buffers on the way.
	static Entity particleCountEntity = INVALID_ENTITY;
	static int    particleCountEdit   = 0;
	if (particleCountEntity != e || !ImGui::IsAnyItemActive())
	{
		particleCountEntity = e;
		particleCountEdit   = (int)c->GetMaxParticleCount();
	}
	ImGui::DragInt("Max particles", &particleCountEdit, 10.0f, 1, 1000000);
	if (ImGui::IsItemDeactivatedAfterEdit())
		c->SetMaxParticleCount((uint32_t)(particleCountEdit < 1 ? 1 : particleCountEdit));
	track("Set Max Particles");
	HelpMarker("Reallocates the particle buffers when you let go, so it also restarts the "
		"simulation.");

	EntityRef(scene, "Emit from mesh", c->meshID);
	HelpMarker("Optional. With a mesh the particles spawn over its surface (or inside it, "
		"with Volume ticked) instead of at the transform origin.");

	if (ImGui::TreeNodeEx("Emission", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::DragFloat("Count / second", &c->count, 0.5f, 0.0f, 100000.0f);
		track("Set Emit Count");
		ImGui::DragFloat("Life", &c->life, 0.01f, 0.0f, 1000.0f);
		track("Set Particle Life");
		ImGui::DragFloat("Life randomness", &c->random_life, 0.005f, 0.0f, 1.0f);
		track("Set Life Randomness");
		ImGui::DragFloat("Size", &c->size, 0.01f, 0.0f, 1000.0f);
		track("Set Particle Size");
		ImGui::DragFloat("Scale X (over life)", &c->scaleX, 0.01f, 0.0f, 100.0f);
		track("Set Particle Scale X");
		ImGui::DragFloat("Scale Y (over life)", &c->scaleY, 0.01f, 0.0f, 100.0f);
		track("Set Particle Scale Y");
		ImGui::DragFloat("Rotation", &c->rotation, 0.01f, -100.0f, 100.0f);
		track("Set Particle Rotation");
		ImGui::DragFloat("Position randomness", &c->random_factor, 0.01f, 0.0f, 100.0f);
		track("Set Position Randomness");
		ImGui::DragFloat("Normal factor", &c->normal_factor, 0.01f, -100.0f, 100.0f);
		track("Set Normal Factor");
		HelpMarker("How much of the emitting surface normal goes into the starting velocity.");
		ImGui::DragFloat("Color randomness", &c->random_color, 0.005f, 0.0f, 1.0f);
		track("Set Color Randomness");
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Motion", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::DragFloat3("Start velocity", &c->velocity.x, 0.01f);
		track("Set Start Velocity");
		ImGui::DragFloat3("Gravity", &c->gravity.x, 0.01f);
		track("Set Particle Gravity");
		ImGui::DragFloat("Drag", &c->drag, 0.005f, 0.0f, 1.0f);
		track("Set Particle Drag");
		HelpMarker("Per-frame velocity multiplier: below 1 slows particles down over time.");
		ImGui::DragFloat("Restitution", &c->restitution, 0.005f, 0.0f, 1.0f);
		track("Set Particle Restitution");
		ImGui::DragFloat("Mass", &c->mass, 0.01f, 0.0f, 1000.0f);
		track("Set Particle Mass");
		ImGui::DragFloat("Motion blur", &c->motionBlurAmount, 0.01f, 0.0f, 10.0f);
		track("Set Particle Motion Blur");
		ImGui::DragFloat("Fixed timestep", &c->FIXED_TIMESTEP, 0.001f, -1.0f, 1.0f);
		track("Set Particle Timestep");
		HelpMarker("-1 uses the variable frame delta.");
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Flags", ImGuiTreeNodeFlags_DefaultOpen))
	{
		FlagCheckbox("Paused", [&] { return c->IsPaused(); }, [&](bool v) { c->SetPaused(v); });
		FlagCheckbox("Debug", [&] { return c->IsDebug(); }, [&](bool v) { c->SetDebug(v); });
		FlagCheckbox("Sorting", [&] { return c->IsSorted(); }, [&](bool v) { c->SetSorted(v); });
		FlagCheckbox("Depth collision", [&] { return c->IsDepthCollisionEnabled(); },
			[&](bool v) { c->SetDepthCollisionEnabled(v); });
		FlagCheckbox("SPH fluid simulation", [&] { return c->IsSPHEnabled(); },
			[&](bool v) { c->SetSPHEnabled(v); });
		FlagCheckbox("Emit from volume", [&] { return c->IsVolumeEnabled(); },
			[&](bool v) { c->SetVolumeEnabled(v); });
		FlagCheckbox("Frame blending", [&] { return c->IsFrameBlendingEnabled(); },
			[&](bool v) { c->SetFrameBlendingEnabled(v); });
		FlagCheckbox("Colliders disabled", [&] { return c->IsCollidersDisabled(); },
			[&](bool v) { c->SetCollidersDisabled(v); });
		FlagCheckbox("Take color from mesh", [&] { return c->IsTakeColorFromMesh(); },
			[&](bool v) { c->SetTakeColorFromMesh(v); });
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Opacity curve"))
	{
		float peakStart = c->opacityCurveControlPeakStart;
		float peakEnd   = c->opacityCurveControlPeakEnd;
		bool changed = ImGui::DragFloat("Peak start", &peakStart, 0.005f, 0.0f, 1.0f);
		track("Set Opacity Peak Start");
		changed |= ImGui::DragFloat("Peak end", &peakEnd, 0.005f, 0.0f, 1.0f);
		track("Set Opacity Peak End");
		if (changed)
			c->SetOpacityCurveControl(peakStart, peakEnd);   // also rebuilds the curve texture
		HelpMarker("Fractions of the particle lifetime where it is fully opaque; it fades in "
			"before the start and out after the end.");
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Sprite sheet"))
	{
		DragUint("Frames X", c->framesX);
		track("Set Frames X");
		DragUint("Frames Y", c->framesY);
		track("Set Frames Y");
		DragUint("Frame count", c->frameCount);
		track("Set Frame Count");
		DragUint("Frame start", c->frameStart);
		track("Set Frame Start");
		ImGui::DragFloat("Frame rate", &c->frameRate, 0.1f, 0.0f, 240.0f);
		track("Set Particle Frame Rate");
		ImGui::TreePop();
	}

	if (c->IsSPHEnabled() && ImGui::TreeNode("SPH fluid"))
	{
		ImGui::DragFloat("Smoothing radius (h)", &c->SPH_h, 0.01f, 0.0f, 100.0f);
		track("Set SPH Radius");
		ImGui::DragFloat("Pressure constant (K)", &c->SPH_K, 1.0f, 0.0f, 100000.0f);
		track("Set SPH Pressure");
		ImGui::DragFloat("Reference density (p0)", &c->SPH_p0, 0.01f, 0.0f, 1000.0f);
		track("Set SPH Density");
		ImGui::DragFloat("Viscosity (e)", &c->SPH_e, 0.001f, 0.0f, 10.0f);
		track("Set SPH Viscosity");
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Runtime"))
	{
		ImGui::Text("Alive: %u", c->statistics.aliveCount);
		ImGui::Text("Dead: %u", c->statistics.deadCount);
		ImGui::Text("Memory: %llu bytes", (unsigned long long)c->GetMemorySizeInBytes());
		if (ImGui::Button("Restart")) c->Restart();
		ImGui::SameLine();
		if (ImGui::Button("Burst 100")) c->Burst(100);
		ImGui::TreePop();
	}
}

// hair particles

void DrawHair(Scene& scene, Entity e, st::EditorHistory* history)
{
	wi::HairParticleSystem* c = scene.hairs.GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::hairs", "Hair Particles", false, history)) return;
	const Edits track{ scene, e, history };

	// Every one of these changes the generated geometry, so each one sets the dirty flag that
	//	makes the hair system rebuild the strands on the next update.
	bool dirty = false;

	EntityRef(scene, "Grow on mesh", c->meshID);
	HelpMarker("Strands are distributed over the triangles of this mesh.");

	dirty |= DragUint("Strand count", c->strandCount, 10.0f);
	track("Set Strand Count");
	dirty |= DragUint("Segments per strand", c->segmentCount);
	track("Set Segment Count");
	dirty |= DragUint("Billboards per segment", c->billboardCount);
	track("Set Billboard Count");
	dirty |= DragUint("Random seed", c->randomSeed);
	track("Set Hair Seed");

	dirty |= ImGui::DragFloat("Length", &c->length, 0.01f, 0.0f, 1000.0f);
	track("Set Hair Length");
	dirty |= ImGui::DragFloat("Width", &c->width, 0.005f, 0.0f, 100.0f);
	track("Set Hair Width");
	dirty |= ImGui::DragFloat("Uniformity", &c->uniformity, 0.005f, 0.0f, 1.0f);
	track("Set Hair Uniformity");
	dirty |= ImGui::DragFloat("Randomness", &c->randomness, 0.005f, 0.0f, 1.0f);
	track("Set Hair Randomness");

	ImGui::DragFloat("Stiffness", &c->stiffness, 0.005f, 0.0f, 100.0f);
	track("Set Hair Stiffness");
	ImGui::DragFloat("Drag", &c->drag, 0.005f, 0.0f, 1.0f);
	track("Set Hair Drag");
	ImGui::DragFloat("Gravity power", &c->gravityPower, 0.01f, -100.0f, 100.0f);
	track("Set Hair Gravity");
	ImGui::DragFloat("View distance", &c->viewDistance, 1.0f, 0.0f, 1000000.0f);
	track("Set Hair View Distance");

	FlagCheckbox("Camera bend", [&] { return c->IsCameraBendEnabled(); },
		[&](bool v) { c->SetCameraBendEnabled(v); });
	HelpMarker("Bends the billboards to face the camera, which keeps thin strands from "
		"disappearing edge-on.");

	if (!c->atlas_rects.empty() && ImGui::TreeNode("Atlas rects"))
	{
		for (size_t i = 0; i < c->atlas_rects.size(); ++i)
		{
			ImGui::PushID((int)i);
			ImGui::DragFloat4("texMulAdd", &c->atlas_rects[i].texMulAdd.x, 0.005f);
			track("Set Hair Atlas Rect");
			ImGui::DragFloat("size", &c->atlas_rects[i].size, 0.005f, 0.0f, 100.0f);
			track("Set Hair Atlas Size");
			ImGui::PopID();
		}
		ImGui::TreePop();
	}

	ImGui::Text("%u particles, %u vertices", c->GetParticleCount(), c->GetVertexCount());
	ImGui::Text("Memory: %llu bytes", (unsigned long long)c->GetMemorySizeInBytes());

	if (dirty)
		c->SetDirty();
}

// weather

void DrawWeather(Scene& scene, Entity e, st::EditorHistory* history)
{
	WeatherComponent* w = scene.weathers.GetComponent(e);
	if (!w) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::weathers", "Weather", false, history)) return;
	const Edits track{ scene, e, history };

	if (ImGui::TreeNodeEx("Sky", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::ColorEdit3("Sun color", &w->sunColor.x);
		track("Set Sun Color");
		ImGui::DragFloat3("Sun direction", &w->sunDirection.x, 0.01f, -1.0f, 1.0f);
		track("Set Sun Direction");
		HelpMarker("Not authored directly in most scenes: a directional light writes this "
			"every frame when there is one.");
		ImGui::DragFloat("Sky exposure", &w->skyExposure, 0.01f, 0.0f, 100.0f);
		track("Set Sky Exposure");
		ImGui::ColorEdit3("Horizon", &w->horizon.x);
		track("Set Horizon Color");
		ImGui::ColorEdit3("Zenith", &w->zenith.x);
		track("Set Zenith Color");
		ImGui::ColorEdit3("Ambient", &w->ambient.x);
		track("Set Ambient Color");
		ImGui::DragFloat("Stars", &w->stars, 0.005f, 0.0f, 1.0f);
		track("Set Stars");
		DragAngle("Sky rotation", w->sky_rotation);
		track("Set Sky Rotation");
		if (AssetDropField("Sky map", w->skyMapName) || ImGui::IsItemDeactivatedAfterEdit())
			w->skyMap = {};   // reloaded by the scene update when the name changes
		track("Set Sky Map");
		if (AssetDropField("Color grading LUT", w->colorGradingMapName) ||
			ImGui::IsItemDeactivatedAfterEdit())
			w->colorGradingMap = {};
		track("Set Color Grading LUT");
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Fog", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::DragFloat("Fog start", &w->fogStart, 0.5f, 0.0f, 1000000.0f);
		track("Set Fog Start");
		ImGui::DragFloat("Fog density", &w->fogDensity, 0.001f, 0.0f, 1.0f);
		track("Set Fog Density");
		FlagCheckbox("Height fog", [&] { return w->IsHeightFog(); },
			[&](bool v) { w->SetHeightFog(v); });
		if (w->IsHeightFog())
		{
			ImGui::DragFloat("Height start", &w->fogHeightStart, 0.1f);
			track("Set Fog Height Start");
			ImGui::DragFloat("Height end", &w->fogHeightEnd, 0.1f);
			track("Set Fog Height End");
		}
		FlagCheckbox("Override fog color", [&] { return w->IsOverrideFogColor(); },
			[&](bool v) { w->SetOverrideFogColor(v); });
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Wind"))
	{
		ImGui::DragFloat3("Direction", &w->windDirection.x, 0.01f);
		track("Set Wind Direction");
		ImGui::DragFloat("Speed", &w->windSpeed, 0.01f, 0.0f, 100.0f);
		track("Set Wind Speed");
		ImGui::DragFloat("Wave size", &w->windWaveSize, 0.01f, 0.0f, 100.0f);
		track("Set Wind Wave Size");
		ImGui::DragFloat("Randomness", &w->windRandomness, 0.01f, 0.0f, 100.0f);
		track("Set Wind Randomness");
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Gravity"))
	{
		ImGui::DragFloat3("Gravity", &w->gravity.x, 0.05f);
		track("Set Scene Gravity");
		HelpMarker("Scene-wide physics gravity, not a per-body setting.");
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Rain"))
	{
		ImGui::DragFloat("Amount", &w->rain_amount, 0.005f, 0.0f, 1.0f);
		track("Set Rain Amount");
		ImGui::DragFloat("Length", &w->rain_length, 0.001f, 0.0f, 1.0f);
		track("Set Rain Length");
		ImGui::DragFloat("Speed", &w->rain_speed, 0.01f, 0.0f, 100.0f);
		track("Set Rain Speed");
		ImGui::DragFloat("Scale", &w->rain_scale, 0.001f, 0.0f, 1.0f);
		track("Set Rain Scale");
		ImGui::DragFloat("Splash scale", &w->rain_splash_scale, 0.005f, 0.0f, 10.0f);
		track("Set Rain Splash Scale");
		ImGui::ColorEdit4("Color", &w->rain_color.x);
		track("Set Rain Color");
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Realistic sky"))
	{
		FlagCheckbox("Enabled", [&] { return w->IsRealisticSky(); },
			[&](bool v) { w->SetRealisticSky(v); });
		FlagCheckbox("Aerial perspective", [&] { return w->IsRealisticSkyAerialPerspective(); },
			[&](bool v) { w->SetRealisticSkyAerialPerspective(v); });
		FlagCheckbox("High quality", [&] { return w->IsRealisticSkyHighQuality(); },
			[&](bool v) { w->SetRealisticSkyHighQuality(v); });
		FlagCheckbox("Receive shadow", [&] { return w->IsRealisticSkyReceiveShadow(); },
			[&](bool v) { w->SetRealisticSkyReceiveShadow(v); });

		AtmosphereParameters& a = w->atmosphereParameters;
		ImGui::DragFloat("Planet bottom radius (km)", &a.bottomRadius, 1.0f, 1.0f, 100000.0f);
		track("Set Planet Radius");
		ImGui::DragFloat("Atmosphere top radius (km)", &a.topRadius, 1.0f, 1.0f, 100000.0f);
		track("Set Atmosphere Radius");
		ImGui::DragFloat3("Planet center", &a.planetCenter.x, 1.0f);
		track("Set Planet Center");
		ImGui::DragFloat3("Rayleigh scattering", &a.rayleighScattering.x, 0.0005f, 0.0f, 1.0f, "%.5f");
		track("Set Rayleigh Scattering");
		ImGui::DragFloat("Rayleigh density scale", &a.rayleighDensityExpScale, 0.001f);
		track("Set Rayleigh Density");
		ImGui::DragFloat3("Mie scattering", &a.mieScattering.x, 0.0005f, 0.0f, 1.0f, "%.5f");
		track("Set Mie Scattering");
		ImGui::DragFloat3("Mie extinction", &a.mieExtinction.x, 0.0005f, 0.0f, 1.0f, "%.5f");
		track("Set Mie Extinction");
		ImGui::DragFloat3("Mie absorption", &a.mieAbsorption.x, 0.0005f, 0.0f, 1.0f, "%.5f");
		track("Set Mie Absorption");
		ImGui::DragFloat("Mie phase G", &a.miePhaseG, 0.005f, -0.999f, 0.999f);
		track("Set Mie Phase");
		ImGui::DragFloat3("Absorption extinction", &a.absorptionExtinction.x, 0.0001f, 0.0f, 1.0f, "%.5f");
		track("Set Absorption Extinction");
		ImGui::DragFloat3("Ground albedo", &a.groundAlbedo.x, 0.005f, 0.0f, 1.0f);
		track("Set Ground Albedo");
		ImGui::DragFloat2("Ray march SPP min/max", &a.rayMarchMinMaxSPP.x, 0.25f, 1.0f, 64.0f);
		track("Set Sky Ray March SPP");
		ImGui::DragFloat("Aerial perspective scale", &a.aerialPerspectiveScale, 0.01f, 0.0f, 100.0f);
		track("Set Aerial Perspective Scale");
		if (ImGui::Button("Reset atmosphere to Earth"))
		{
			if (history) history->BeginEntity(scene, e, "Reset Atmosphere");
			a.init();
			if (history) history->Commit(scene);
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Volumetric clouds"))
	{
		FlagCheckbox("Enabled", [&] { return w->IsVolumetricClouds(); },
			[&](bool v) { w->SetVolumetricClouds(v); });
		FlagCheckbox("Cast shadow", [&] { return w->IsVolumetricCloudsCastShadow(); },
			[&](bool v) { w->SetVolumetricCloudsCastShadow(v); });
		FlagCheckbox("Receive shadow", [&] { return w->IsVolumetricCloudsReceiveShadow(); },
			[&](bool v) { w->SetVolumetricCloudsReceiveShadow(v); });

		VolumetricCloudParameters& v = w->volumetricCloudParameters;
		ImGui::DragFloat("Start height", &v.cloudStartHeight, 5.0f, 0.0f, 100000.0f);
		track("Set Cloud Start Height");
		ImGui::DragFloat("Thickness", &v.cloudThickness, 5.0f, 0.0f, 100000.0f);
		track("Set Cloud Thickness");
		ImGui::DragFloat("Beer powder", &v.beerPowder, 0.1f, 0.0f, 1000.0f);
		track("Set Beer Powder");
		ImGui::DragFloat("Beer powder power", &v.beerPowderPower, 0.01f, 0.0f, 10.0f);
		track("Set Beer Powder Power");
		ImGui::DragFloat("Ambient ground multiplier", &v.ambientGroundMultiplier, 0.005f, 0.0f, 1.0f);
		track("Set Cloud Ambient Ground");
		ImGui::DragFloat("Phase G", &v.phaseG, 0.005f, -0.999f, 0.999f);
		track("Set Cloud Phase G");
		ImGui::DragFloat("Phase G2", &v.phaseG2, 0.005f, -0.999f, 0.999f);
		track("Set Cloud Phase G2");
		ImGui::DragFloat("Phase blend", &v.phaseBlend, 0.005f, 0.0f, 1.0f);
		track("Set Cloud Phase Blend");
		ImGui::DragFloat("Animation multiplier", &v.animationMultiplier, 0.01f, 0.0f, 100.0f);
		track("Set Cloud Animation");
		ImGui::DragFloat("Horizon blend amount", &v.horizonBlendAmount, 0.000005f, 0.0f, 1.0f, "%.6f");
		track("Set Cloud Horizon Blend");
		ImGui::DragFloat("Horizon blend power", &v.horizonBlendPower, 0.01f, 0.0f, 10.0f);
		track("Set Cloud Horizon Power");

		auto layer = [&](const char* label, VolumetricCloudLayer& L) {
			if (!ImGui::TreeNode(label)) return;
			ImGui::ColorEdit3("Albedo", &L.albedo.x);
			track("Set Cloud Albedo");
			ImGui::DragFloat3("Extinction", &L.extinctionCoefficient.x, 0.001f, 0.0f, 10.0f, "%.4f");
			track("Set Cloud Extinction");
			ImGui::DragFloat("Coverage amount", &L.coverageAmount, 0.005f, 0.0f, 1.0f);
			track("Set Cloud Coverage");
			ImGui::DragFloat("Coverage minimum", &L.coverageMinimum, 0.005f, 0.0f, 1.0f);
			track("Set Cloud Coverage Min");
			ImGui::DragFloat("Type amount", &L.typeAmount, 0.005f, 0.0f, 1.0f);
			track("Set Cloud Type Amount");
			ImGui::DragFloat("Type minimum", &L.typeMinimum, 0.005f, 0.0f, 1.0f);
			track("Set Cloud Type Min");
			ImGui::DragFloat("Rain amount", &L.rainAmount, 0.005f, 0.0f, 1.0f);
			track("Set Cloud Rain Amount");
			ImGui::DragFloat("Total noise scale", &L.totalNoiseScale, 0.00001f, 0.0f, 1.0f, "%.6f");
			track("Set Cloud Noise Scale");
			ImGui::DragFloat("Detail scale", &L.detailScale, 0.01f, 0.0f, 100.0f);
			track("Set Cloud Detail Scale");
			ImGui::DragFloat("Curl scale", &L.curlScale, 0.005f, 0.0f, 100.0f);
			track("Set Cloud Curl Scale");
			ImGui::DragFloat("Weather scale", &L.weatherScale, 0.000005f, 0.0f, 1.0f, "%.6f");
			track("Set Cloud Weather Scale");
			ImGui::DragFloat("Wind speed", &L.windSpeed, 0.1f, 0.0f, 1000.0f);
			track("Set Cloud Wind Speed");
			ImGui::DragFloat("Wind angle", &L.windAngle, 0.01f, -XM_2PI, XM_2PI);
			track("Set Cloud Wind Angle");
			ImGui::DragFloat("Wind up amount", &L.windUpAmount, 0.005f, -10.0f, 10.0f);
			track("Set Cloud Wind Up");
			ImGui::DragFloat4("Gradient small", &L.gradientSmall.x, 0.005f, 0.0f, 1.0f);
			track("Set Cloud Gradient Small");
			ImGui::DragFloat4("Gradient medium", &L.gradientMedium.x, 0.005f, 0.0f, 1.0f);
			track("Set Cloud Gradient Medium");
			ImGui::DragFloat4("Gradient large", &L.gradientLarge.x, 0.005f, 0.0f, 1.0f);
			track("Set Cloud Gradient Large");
			ImGui::TreePop();
		};
		layer("Layer 1", v.layerFirst);
		layer("Layer 2", v.layerSecond);

		if (ImGui::TreeNode("Performance"))
		{
			ImGui::DragInt("Max step count", &v.maxStepCount, 1.0f, 1, 1024);
			track("Set Cloud Step Count");
			ImGui::DragFloat("Max marching distance", &v.maxMarchingDistance, 100.0f, 0.0f, 1000000.0f);
			track("Set Cloud March Distance");
			ImGui::DragFloat("Render distance", &v.renderDistance, 100.0f, 0.0f, 1000000.0f);
			track("Set Cloud Render Distance");
			ImGui::DragFloat("LOD distance", &v.LODDistance, 100.0f, 0.0f, 1000000.0f);
			track("Set Cloud LOD Distance");
			ImGui::DragFloat("Big step march", &v.bigStepMarch, 0.05f, 0.0f, 100.0f);
			track("Set Cloud Big Step");
			ImGui::DragFloat("Transmittance threshold", &v.transmittanceThreshold, 0.001f, 0.0f, 1.0f);
			track("Set Cloud Transmittance Threshold");
			ImGui::DragFloat("Shadow sample count", &v.shadowSampleCount, 0.5f, 0.0f, 128.0f);
			track("Set Cloud Shadow Samples");
			ImGui::DragFloat("Shadow step length", &v.shadowStepLength, 10.0f, 0.0f, 100000.0f);
			track("Set Cloud Shadow Step");
			ImGui::TreePop();
		}

		if (AssetDropField("Weather map 1", w->volumetricCloudsWeatherMapFirstName) ||
			ImGui::IsItemDeactivatedAfterEdit())
			w->volumetricCloudsWeatherMapFirst = {};
		track("Set Cloud Weather Map 1");
		if (AssetDropField("Weather map 2", w->volumetricCloudsWeatherMapSecondName) ||
			ImGui::IsItemDeactivatedAfterEdit())
			w->volumetricCloudsWeatherMapSecond = {};
		track("Set Cloud Weather Map 2");
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Ocean"))
	{
		FlagCheckbox("Enabled", [&] { return w->IsOceanEnabled(); },
			[&](bool v) { w->SetOceanEnabled(v); });

		wi::Ocean::OceanParameters& o = w->oceanParameters;
		ImGui::DragInt("Displacement map dim", &o.dmap_dim, 1.0f, 32, 2048);
		track("Set Ocean Map Dim");
		HelpMarker("Must be a power of two; the simulation is an FFT over this grid.");
		ImGui::DragFloat("Patch length", &o.patch_length, 0.5f, 1.0f, 100000.0f);
		track("Set Ocean Patch Length");
		ImGui::DragFloat("Time scale", &o.time_scale, 0.005f, 0.0f, 10.0f);
		track("Set Ocean Time Scale");
		ImGui::DragFloat("Wave amplitude", &o.wave_amplitude, 1.0f, 0.0f, 100000.0f);
		track("Set Ocean Wave Amplitude");
		ImGui::DragFloat2("Wind direction", &o.wind_dir.x, 0.01f);
		track("Set Ocean Wind Direction");
		ImGui::DragFloat("Wind speed", &o.wind_speed, 1.0f, 0.0f, 10000.0f);
		track("Set Ocean Wind Speed");
		ImGui::DragFloat("Wind dependency", &o.wind_dependency, 0.005f, 0.0f, 1.0f);
		track("Set Ocean Wind Dependency");
		ImGui::DragFloat("Choppy scale", &o.choppy_scale, 0.01f, 0.0f, 10.0f);
		track("Set Ocean Choppy Scale");
		ImGui::ColorEdit4("Water color", &o.waterColor.x);
		track("Set Water Color");
		ImGui::ColorEdit4("Extinction color", &o.extinctionColor.x);
		track("Set Water Extinction Color");
		ImGui::DragFloat("Water height", &o.waterHeight, 0.05f);
		track("Set Water Height");
		DragUint("Surface detail", o.surfaceDetail);
		track("Set Ocean Surface Detail");
		ImGui::DragFloat("Displacement tolerance", &o.surfaceDisplacementTolerance, 0.05f, 0.0f, 100.0f);
		track("Set Ocean Displacement Tolerance");
		ImGui::TreePop();
	}
}

// sound

void DrawSound(Scene& scene, Entity e, st::EditorHistory* history)
{
	SoundComponent* c = scene.sounds.GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::sounds", "Sound", false, history)) return;
	const Edits track{ scene, e, history };

	AssetDropField("File", c->filename);
	track("Set Sound File");
	HelpMarker("Reload the scene, or re-add the component, after changing this: the resource "
		"is opened when the component is created.");

	ImGui::DragFloat("Volume", &c->volume, 0.005f, 0.0f, 2.0f);
	track("Set Sound Volume");

	if (ImGui::Button(c->IsPlaying() ? "Stop" : "Play"))
		c->IsPlaying() ? c->Stop() : c->Play();
	ImGui::SameLine();
	ImGui::TextDisabled(c->IsPlaying() ? "playing" : "stopped");

	FlagCheckbox("Looped", [&] { return c->IsLooped(); }, [&](bool v) { c->SetLooped(v); });
	FlagCheckbox("Disable 3D", [&] { return c->IsDisable3D(); }, [&](bool v) { c->SetDisable3D(v); });
	HelpMarker("Plays at a fixed volume regardless of where the listener is.");

	ImGui::TextDisabled(c->soundResource.IsValid() ? "resource: loaded" : "resource: missing");
}

// video

void DrawVideo(Scene& scene, Entity e, st::EditorHistory* history)
{
	VideoComponent* c = scene.videos.GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::videos", "Video", false, history)) return;
	const Edits track{ scene, e, history };

	AssetDropField("File", c->filename);
	track("Set Video File");

	const float length = c->GetLength();
	float timer = c->currentTimer;
	if (ImGui::SliderFloat("Time", &timer, 0.0f, length > 0.0f ? length : 1.0f, "%.2f s"))
		c->Seek(timer);
	track("Seek Video");
	ImGui::Text("Length: %.2f s", length);

	if (ImGui::Button(c->IsPlaying() ? "Pause" : "Play"))
		c->IsPlaying() ? c->Pause() : c->Play();
	ImGui::SameLine();
	if (ImGui::Button("Stop")) c->Stop();
	ImGui::SameLine();
	ImGui::TextDisabled(c->IsPlaying() ? "playing" : "stopped");

	FlagCheckbox("Looped", [&] { return c->IsLooped(); }, [&](bool v) { c->SetLooped(v); });
	ImGui::TextDisabled(c->videoResource.IsValid() ? "resource: loaded" : "resource: missing");
}

// inverse kinematics

void DrawIK(Scene& scene, Entity e, st::EditorHistory* history)
{
	InverseKinematicsComponent* c = scene.inverse_kinematics.GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::inverse_kinematics", "Inverse Kinematics", false, history)) return;
	const Edits track{ scene, e, history };

	EntityRef(scene, "Target", c->target);
	HelpMarker("The entity this chain reaches for. It needs a TransformComponent.");

	DragUint("Chain length", c->chain_length);
	track("Set IK Chain Length");
	HelpMarker("How many parents up from this entity the solver is allowed to rotate.");
	DragUint("Iterations", c->iteration_count);
	track("Set IK Iterations");
	HelpMarker("A longer chain needs more iterations to converge.");

	FlagCheckbox("Disabled", [&] { return c->IsDisabled(); }, [&](bool v) { c->SetDisabled(v); });

	ImGui::Separator();
	ImGui::Checkbox("Use target position", &c->use_target_position);
	HelpMarker("Not serialized: a runtime override that aims at a raw position instead of "
		"the target entity.");
	if (c->use_target_position)
		ImGui::DragFloat3("Target position", &c->target_position.x, 0.01f);
}

// spring

void DrawSpring(Scene& scene, Entity e, st::EditorHistory* history)
{
	SpringComponent* c = scene.springs.GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::springs", "Spring", false, history)) return;
	const Edits track{ scene, e, history };

	ImGui::DragFloat("Stiffness", &c->stiffnessForce, 0.005f, 0.0f, 10.0f);
	track("Set Spring Stiffness");
	ImGui::DragFloat("Drag", &c->dragForce, 0.005f, 0.0f, 10.0f);
	track("Set Spring Drag");
	ImGui::DragFloat("Wind", &c->windForce, 0.005f, 0.0f, 10.0f);
	track("Set Spring Wind");
	ImGui::DragFloat("Hit radius", &c->hitRadius, 0.005f, 0.0f, 100.0f);
	track("Set Spring Hit Radius");
	HelpMarker("Radius used against ColliderComponents, so a bone chain does not sink into "
		"the body it hangs off.");

	FlagCheckbox("Gravity enabled", [&] { return c->IsGravityEnabled(); },
		[&](bool v) { c->SetGravityEnabled(v); });
	if (c->IsGravityEnabled())
	{
		ImGui::DragFloat3("Gravity direction", &c->gravityDir.x, 0.01f, -1.0f, 1.0f);
		track("Set Spring Gravity Direction");
		ImGui::DragFloat("Gravity power", &c->gravityPower, 0.005f, -10.0f, 10.0f);
		track("Set Spring Gravity Power");
	}

	FlagCheckbox("Disabled", [&] { return c->IsDisabled(); }, [&](bool v) { c->SetDisabled(v); });
	if (ImGui::Button("Reset simulation")) c->Reset();
	ImGui::SameLine();
	ImGui::TextDisabled("%zu chained children", c->children.size());
}

// collider

void DrawCollider(Scene& scene, Entity e, st::EditorHistory* history)
{
	ColliderComponent* c = scene.colliders.GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::colliders", "Collider", false, history)) return;
	const Edits track{ scene, e, history };

	static const char* shapes[] = { "Sphere", "Capsule", "Plane" };
	int shape = (int)c->shape;
	if (ImGui::Combo("Shape", &shape, shapes, IM_ARRAYSIZE(shapes)))
		c->shape = (ColliderComponent::Shape)shape;
	track("Set Collider Shape");
	HelpMarker("This is the LIGHTWEIGHT collider springs, hair and capsule shadows test "
		"against, not a rigid body.");

	ImGui::DragFloat("Radius", &c->radius, 0.005f, 0.0f, 1000.0f);
	track("Set Collider Radius");
	ImGui::DragFloat3("Offset", &c->offset.x, 0.01f);
	track("Set Collider Offset");
	if (c->shape == ColliderComponent::Shape::Capsule)
	{
		ImGui::DragFloat3("Tail", &c->tail.x, 0.01f);
		track("Set Collider Tail");
		HelpMarker("The far end of the capsule, in the same local space as the offset.");
	}

	FlagCheckbox("CPU (springs, hair)", [&] { return c->IsCPUEnabled(); },
		[&](bool v) { c->SetCPUEnabled(v); });
	FlagCheckbox("GPU (particles)", [&] { return c->IsGPUEnabled(); },
		[&](bool v) { c->SetGPUEnabled(v); });
	FlagCheckbox("Capsule shadow", [&] { return c->IsCapsuleShadowEnabled(); },
		[&](bool v) { c->SetCapsuleShadowEnabled(v); });
}

// script

void DrawScript(Scene& scene, Entity e, st::EditorHistory* history)
{
	ScriptComponent* c = scene.scripts.GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::scripts", "Script", false, history)) return;
	const Edits track{ scene, e, history };

	// The compiled binary belongs to the OLD file, so it has to be reloaded rather than left
	//	in place: without this the component keeps running the previous script. On commit,
	//	because every keystroke would otherwise try to compile a half-typed path.
	if (AssetDropField("File", c->filename))
		c->CreateFromFile(c->filename);
	else if (ImGui::IsItemDeactivatedAfterEdit())
		c->CreateFromFile(c->filename);
	track("Set Script File");

	if (ImGui::Button(c->IsPlaying() ? "Stop" : "Play"))
		c->IsPlaying() ? c->Stop() : c->Play();
	ImGui::SameLine();
	ImGui::TextDisabled(c->IsPlaying() ? "playing" : "stopped");

	FlagCheckbox("Play once", [&] { return c->IsPlayingOnlyOnce(); },
		[&](bool v) { c->SetPlayOnce(v); });

	if (ImGui::Button("Reload"))
	{
		if (history) history->BeginEntity(scene, e, "Reload Script");
		c->CreateFromFile(c->filename);
		if (history) history->Commit(scene);
	}
	ImGui::SameLine();
	ImGui::TextDisabled("%zu bytes compiled", c->script.size());
}

// expression

void DrawExpression(Scene& scene, Entity e, st::EditorHistory* history)
{
	ExpressionComponent* c = scene.expressions.GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::expressions", "Expression", false, history)) return;
	const Edits track{ scene, e, history };

	static const char* presetNames[] = {
		"Happy", "Angry", "Sad", "Relaxed", "Surprised",
		"Aa", "Ih", "Ou", "Ee", "Oh",
		"Blink", "Blink left", "Blink right",
		"Look up", "Look down", "Look left", "Look right",
		"Neutral", "(none)",
	};
	static const char* overrideNames[] = { "None", "Block", "Blend" };

	FlagCheckbox("Force talking", [&] { return c->IsForceTalkingEnabled(); },
		[&](bool v) { c->SetForceTalkingEnabled(v); });
	HelpMarker("Runs the mouth animation continuously, even with no voice playing.");

	ImGui::DragFloat("Blink frequency", &c->blink_frequency, 0.005f, 0.0f, 10.0f);
	track("Set Blink Frequency");
	ImGui::DragFloat("Blink length", &c->blink_length, 0.005f, 0.0f, 5.0f);
	track("Set Blink Length");
	ImGui::DragInt("Blink count", &c->blink_count, 0.1f, 0, 10);
	track("Set Blink Count");
	ImGui::DragFloat("Look frequency", &c->look_frequency, 0.005f, 0.0f, 10.0f);
	track("Set Look Frequency");
	ImGui::DragFloat("Look length", &c->look_length, 0.005f, 0.0f, 5.0f);
	track("Set Look Length");

	if (ImGui::TreeNodeEx("Expressions", ImGuiTreeNodeFlags_DefaultOpen))
	{
		for (size_t i = 0; i < c->expressions.size(); ++i)
		{
			ExpressionComponent::Expression& x = c->expressions[i];
			ImGui::PushID((int)i);
			const std::string title = x.name.empty() ? ("Expression " + std::to_string(i)) : x.name;
			if (ImGui::TreeNode(title.c_str()))
			{
				EditString("Name", x.name);
				track("Rename Expression");

				// SetWeight rather than a raw store: it is what raises the dirty flag the
				// expression system uses to re-apply the morph target bindings.
				float weight = x.weight;
				if (ImGui::SliderFloat("Weight", &weight, 0.0f, 1.0f))
					x.SetWeight(weight);
				track("Set Expression Weight");

				int preset = (int)x.preset;
				if (preset >= 0 && preset <= (int)ExpressionComponent::Preset::Count &&
					ImGui::Combo("Preset", &preset, presetNames, IM_ARRAYSIZE(presetNames)))
					x.preset = (ExpressionComponent::Preset)preset;
				track("Set Expression Preset");

				auto overrideCombo = [&](const char* label, ExpressionComponent::Override& o) {
					int v = (int)o;
					if (ImGui::Combo(label, &v, overrideNames, IM_ARRAYSIZE(overrideNames)))
						o = (ExpressionComponent::Override)v;
					track("Set Expression Override");
				};
				overrideCombo("Override mouth", x.override_mouth);
				overrideCombo("Override blink", x.override_blink);
				overrideCombo("Override look",  x.override_look);

				FlagCheckbox("Binary", [&] { return x.IsBinary(); }, [&](bool v) { x.SetBinary(v); });
				HelpMarker("Snaps the weight to 0 or 1 instead of blending.");

				ImGui::Text("%zu morph target bindings", x.morph_target_bindings.size());
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		if (ImGui::Button("Add expression"))
		{
			if (history) history->BeginEntity(scene, e, "Add Expression");
			c->expressions.emplace_back();
			if (history) history->Commit(scene);
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Preset bindings"))
	{
		ImGui::TextDisabled("Which entry in the list above each preset maps to. -1 is unbound.");
		for (int i = 0; i < (int)ExpressionComponent::Preset::Count; ++i)
		{
			ImGui::PushID(i);
			ImGui::DragInt(presetNames[i], &c->presets[i], 0.1f, -1, (int)c->expressions.size() - 1);
			track("Set Preset Binding");
			ImGui::PopID();
		}
		ImGui::TreePop();
	}
}

// humanoid

void DrawHumanoid(Scene& scene, Entity e, st::EditorHistory* history)
{
	HumanoidComponent* c = scene.humanoids.GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::humanoids", "Humanoid", false, history)) return;
	const Edits track{ scene, e, history };

	ImGui::Text("Rig: %s", c->IsValid() ? "valid" : "missing required bones");

	FlagCheckbox("Look at", [&] { return c->IsLookAtEnabled(); },
		[&](bool v) { c->SetLookAtEnabled(v); });
	FlagCheckbox("Ragdoll physics", [&] { return c->IsRagdollPhysicsEnabled(); },
		[&](bool v) { c->SetRagdollPhysicsEnabled(v); });
	FlagCheckbox("Ragdoll disabled", [&] { return c->IsRagdollDisabled(); },
		[&](bool v) { c->SetRagdollDisabled(v); });
	FlagCheckbox("Disable intersection", [&] { return c->IsIntersectionDisabled(); },
		[&](bool v) { c->SetIntersectionDisabled(v); });
	FlagCheckbox("Disable capsule shadow", [&] { return c->IsCapsuleShadowDisabled(); },
		[&](bool v) { c->SetCapsuleShadowDisabled(v); });

	if (ImGui::TreeNodeEx("Look at", ImGuiTreeNodeFlags_DefaultOpen))
	{
		EntityRef(scene, "Look at entity", c->lookAtEntity);
		HelpMarker("Unset means the head follows the free-floating lookAt position instead.");
		ImGui::DragFloat2("Head rotation max (rad)", &c->head_rotation_max.x, 0.01f, 0.0f, XM_PI);
		track("Set Head Rotation Max");
		ImGui::DragFloat2("Eye rotation max (rad)", &c->eye_rotation_max.x, 0.005f, 0.0f, XM_PI);
		track("Set Eye Rotation Max");
		ImGui::DragFloat("Head rotation speed", &c->head_rotation_speed, 0.005f, 0.0f, 10.0f);
		track("Set Head Rotation Speed");
		ImGui::DragFloat("Eye rotation speed", &c->eye_rotation_speed, 0.005f, 0.0f, 10.0f);
		track("Set Eye Rotation Speed");
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Proportions"))
	{
		ImGui::DragFloat("Ragdoll fatness", &c->ragdoll_fatness, 0.005f, 0.1f, 5.0f);
		track("Set Ragdoll Fatness");
		ImGui::DragFloat("Ragdoll head size", &c->ragdoll_headsize, 0.005f, 0.1f, 5.0f);
		track("Set Ragdoll Head Size");
		ImGui::DragFloat("Arm spacing", &c->arm_spacing, 0.005f, -1.0f, 1.0f);
		track("Set Arm Spacing");
		ImGui::DragFloat("Leg spacing", &c->leg_spacing, 0.005f, -1.0f, 1.0f);
		track("Set Leg Spacing");
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Bones"))
	{
		static const char* boneNames[] = {
			"Hips", "Spine", "Chest", "UpperChest", "Neck",
			"Head", "LeftEye", "RightEye", "Jaw",
			"LeftUpperLeg", "LeftLowerLeg", "LeftFoot", "LeftToes",
			"RightUpperLeg", "RightLowerLeg", "RightFoot", "RightToes",
			"LeftShoulder", "LeftUpperArm", "LeftLowerArm", "LeftHand",
			"RightShoulder", "RightUpperArm", "RightLowerArm", "RightHand",
			"LeftThumbMetacarpal", "LeftThumbProximal", "LeftThumbDistal",
			"LeftIndexProximal", "LeftIndexIntermediate", "LeftIndexDistal",
			"LeftMiddleProximal", "LeftMiddleIntermediate", "LeftMiddleDistal",
			"LeftRingProximal", "LeftRingIntermediate", "LeftRingDistal",
			"LeftLittleProximal", "LeftLittleIntermediate", "LeftLittleDistal",
			"RightThumbMetacarpal", "RightThumbProximal", "RightThumbDistal",
			"RightIndexIntermediate", "RightIndexDistal", "RightIndexProximal",
			"RightMiddleProximal", "RightMiddleIntermediate", "RightMiddleDistal",
			"RightRingProximal", "RightRingIntermediate", "RightRingDistal",
			"RightLittleProximal", "RightLittleIntermediate", "RightLittleDistal",
		};
		static_assert(IM_ARRAYSIZE(boneNames) == (int)HumanoidComponent::HumanoidBone::Count,
			"humanoid bone name table is out of step with the enum");
		for (int i = 0; i < (int)HumanoidComponent::HumanoidBone::Count; ++i)
		{
			if (c->bones[i] == INVALID_ENTITY) continue;
			ImGui::BulletText("%s: %s", boneNames[i], EntityLabel(scene, c->bones[i]).c_str());
		}
		ImGui::TextDisabled("Only mapped bones are listed. The mapping comes from the import.");
		ImGui::TreePop();
	}
}

// terrain

void DrawTerrain(Scene& scene, Entity e, st::EditorHistory* history)
{
	wi::terrain::Terrain* t = scene.terrains.GetComponent(e);
	if (!t) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::terrains", "Terrain", false, history)) return;
	const Edits track{ scene, e, history };

	// Terrain generation is a background job over a chunk map. Changing a generation input
	//	does NOT rebuild what is already generated, so the restart button is the other half of
	//	every field in this panel.
	ImGui::TextDisabled(t->IsGenerationBusy() ? "generating..." : "idle");
	if (ImGui::Button("Restart generation"))
	{
		if (history) history->BeginEntity(scene, e, "Restart Terrain");
		t->Generation_Restart();
		if (history) history->Commit(scene);
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) t->Generation_Cancel();
	ImGui::Text("%zu chunks, %zu props", t->chunks.size(), t->props.size());

	if (ImGui::TreeNodeEx("Generation", ImGuiTreeNodeFlags_DefaultOpen))
	{
		DragUint("Seed", t->seed);
		track("Set Terrain Seed");
		ImGui::DragInt("Chunk generation radius", &t->generation, 0.1f, 0, 64);
		track("Set Terrain Generation Radius");
		ImGui::DragInt("Prop generation radius", &t->prop_generation, 0.1f, 0, 64);
		track("Set Prop Generation Radius");
		ImGui::DragInt("Physics generation radius", &t->physics_generation, 0.1f, 0, 64);
		track("Set Physics Generation Radius");
		ImGui::DragInt("Grass chunk distance", &t->grass_chunk_dist, 0.1f, 0, 64);
		track("Set Grass Chunk Distance");
		ImGui::DragFloat("Chunk scale", &t->chunk_scale, 0.01f, 0.01f, 1000.0f);
		track("Set Chunk Scale");
		ImGui::DragFloat("Bottom level", &t->bottomLevel, 0.5f);
		track("Set Terrain Bottom Level");
		ImGui::DragFloat("Top level", &t->topLevel, 0.5f);
		track("Set Terrain Top Level");
		ImGui::DragFloat("Prop density", &t->prop_density, 0.01f, 0.0f, 10.0f);
		track("Set Prop Density");
		ImGui::DragFloat("Grass density", &t->grass_density, 0.01f, 0.0f, 10.0f);
		track("Set Grass Density");
		ImGui::DragFloat("LOD bias", &t->lod_bias, 0.01f);
		track("Set Terrain LOD Bias");
		ImGui::DragFloat("Time budget (ms)", &t->generation_time_budget_milliseconds, 0.1f, 0.0f, 100.0f);
		track("Set Terrain Time Budget");
		HelpMarker("How long the generation job may run per frame before it yields.");
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Regions", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::DragFloat("Region 1", &t->region1, 0.01f, 0.0f, 100.0f);
		track("Set Terrain Region 1");
		ImGui::DragFloat("Region 2", &t->region2, 0.01f, 0.0f, 100.0f);
		track("Set Terrain Region 2");
		ImGui::DragFloat("Region 3", &t->region3, 0.01f, 0.0f, 100.0f);
		track("Set Terrain Region 3");
		HelpMarker("Blend weights between the terrain material layers by slope and height.");
		ImGui::Text("Materials: %zu (splines: %zu)",
			t->materialEntities.size(), t->splineMaterialEntities.size());
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Flags", ImGuiTreeNodeFlags_DefaultOpen))
	{
		FlagCheckbox("Center to camera", [&] { return t->IsCenterToCamEnabled(); },
			[&](bool v) { t->SetCenterToCamEnabled(v); });
		FlagCheckbox("Remove distant chunks", [&] { return t->IsRemovalEnabled(); },
			[&](bool v) { t->SetRemovalEnabled(v); });
		FlagCheckbox("Grass", [&] { return t->IsGrassEnabled(); },
			[&](bool v) { t->SetGrassEnabled(v); });
		FlagCheckbox("Physics", [&] { return t->IsPhysicsEnabled(); },
			[&](bool v) { t->SetPhysicsEnabled(v); });
		FlagCheckbox("Tessellation", [&] { return t->IsTessellationEnabled(); },
			[&](bool v) { t->SetTessellationEnabled(v); });
		ImGui::TreePop();
	}

	ImGui::DragInt("Chunk GPU upload range", &t->chunk_buffer_range, 0.1f, 1, 16);
	track("Set Chunk Upload Range");
	ImGui::Text("Modifiers: %zu", t->modifiers.size());
	ImGui::TextDisabled("The terrain weather block and the grass material are edited through "
		"the entities the generator creates, not from here.");
}

// sprite

void DrawSprite(Scene& scene, Entity e, st::EditorHistory* history)
{
	wi::Sprite* s = scene.sprites.GetComponent(e);
	if (!s) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::sprites", "Sprite", false, history)) return;
	const Edits track{ scene, e, history };

	AssetDropField("Texture", s->textureName);
	track("Set Sprite Texture");
	AssetDropField("Mask", s->maskName);
	track("Set Sprite Mask");
	HelpMarker("Names are resolved when the scene loads; reload after changing one.");

	FlagCheckbox("Hidden", [&] { return s->IsHidden(); }, [&](bool v) { s->SetHidden(v); });
	FlagCheckbox("Disable update", [&] { return s->IsDisableUpdate(); },
		[&](bool v) { s->SetDisableUpdate(v); });
	FlagCheckbox("Camera facing", [&] { return s->IsCameraFacing(); },
		[&](bool v) { s->SetCameraFacing(v); });
	FlagCheckbox("Camera scaling", [&] { return s->IsCameraScaling(); },
		[&](bool v) { s->SetCameraScaling(v); });

	wi::image::Params& p = s->params;
	if (ImGui::TreeNodeEx("Draw parameters", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::DragFloat3("Position", &p.pos.x, 0.5f);
		track("Set Sprite Position");
		ImGui::DragFloat2("Size", &p.siz.x, 0.5f);
		track("Set Sprite Size");
		ImGui::DragFloat2("Scale", &p.scale.x, 0.01f);
		track("Set Sprite Scale");
		ImGui::DragFloat2("Pivot", &p.pivot.x, 0.01f);
		track("Set Sprite Pivot");
		HelpMarker("(0,0) top left, (0.5,0.5) center, (1,1) bottom right.");
		DragAngle("Rotation", p.rotation);
		track("Set Sprite Rotation");
		ImGui::ColorEdit4("Color", &p.color.x);
		track("Set Sprite Color");
		ImGui::DragFloat("Opacity", &p.opacity, 0.005f, 0.0f, 1.0f);
		track("Set Sprite Opacity");
		ImGui::DragFloat("Fade", &p.fade, 0.005f, 0.0f, 1.0f);
		track("Set Sprite Fade");
		ImGui::DragFloat("Intensity", &p.intensity, 0.01f, 0.0f, 100.0f);
		track("Set Sprite Intensity");
		ImGui::DragFloat("Saturation", &p.saturation, 0.005f, 0.0f, 2.0f);
		track("Set Sprite Saturation");
		ImGui::DragFloat("Border soften", &p.border_soften, 0.005f, 0.0f, 1.0f);
		track("Set Sprite Border Soften");
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Texture mapping"))
	{
		ImGui::DragFloat4("texMulAdd", &p.texMulAdd.x, 0.005f);
		track("Set Sprite TexMulAdd");
		ImGui::DragFloat4("texMulAdd2", &p.texMulAdd2.x, 0.005f);
		track("Set Sprite TexMulAdd2");
		ImGui::DragFloat2("Tex offset", &p.texOffset.x, 0.005f);
		track("Set Sprite Tex Offset");
		ImGui::DragFloat2("Tex offset 2", &p.texOffset2.x, 0.005f);
		track("Set Sprite Tex Offset 2");
		ImGui::DragFloat("Mask alpha range start", &p.mask_alpha_range_start, 0.005f, 0.0f, 1.0f);
		track("Set Mask Alpha Start");
		ImGui::DragFloat("Mask alpha range end", &p.mask_alpha_range_end, 0.005f, 0.0f, 1.0f);
		track("Set Mask Alpha End");
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Blending / stencil"))
	{
		static const char* blendModes[] = { "Opaque", "Alpha", "Premultiplied", "Additive", "Multiply", "Inverse" };
		int blend = (int)p.blendFlag;
		if (ImGui::Combo("Blend", &blend, blendModes, IM_ARRAYSIZE(blendModes)))
			p.blendFlag = (wi::enums::BLENDMODE)blend;
		track("Set Sprite Blend Mode");

		static const char* sampleModes[] = { "Clamp", "Wrap", "Mirror" };
		int sample = (int)p.sampleFlag;
		if (ImGui::Combo("Sampling", &sample, sampleModes, IM_ARRAYSIZE(sampleModes)))
			p.sampleFlag = (wi::image::SAMPLEMODE)sample;
		track("Set Sprite Sample Mode");

		static const char* qualities[] = { "Nearest", "Linear", "Anisotropic" };
		int quality = (int)p.quality;
		if (ImGui::Combo("Quality", &quality, qualities, IM_ARRAYSIZE(qualities)))
			p.quality = (wi::image::QUALITY)quality;
		track("Set Sprite Quality");

		static const char* stencilModes[] = { "Disabled", "Equal", "Less", "Less or equal",
			"Greater", "Greater or equal", "Not", "Always" };
		int stencil = (int)p.stencilComp;
		if (ImGui::Combo("Stencil test", &stencil, stencilModes, IM_ARRAYSIZE(stencilModes)))
			p.stencilComp = (wi::image::STENCILMODE)stencil;
		track("Set Sprite Stencil Test");

		static const char* refModes[] = { "Engine", "User", "All" };
		int refMode = (int)p.stencilRefMode;
		if (ImGui::Combo("Stencil ref mode", &refMode, refModes, IM_ARRAYSIZE(refModes)))
			p.stencilRefMode = (wi::image::STENCILREFMODE)refMode;
		track("Set Sprite Stencil Ref Mode");

		int ref = (int)p.stencilRef;
		if (ImGui::SliderInt("Stencil ref", &ref, 0, 255))
			p.stencilRef = (uint8_t)ref;
		track("Set Sprite Stencil Ref");
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Draw flags"))
	{
		FlagCheckbox("Mirror", [&] { return p.isMirrorEnabled(); },
			[&](bool v) { v ? p.enableMirror() : p.disableMirror(); });
		FlagCheckbox("Full screen", [&] { return p.isFullScreenEnabled(); },
			[&](bool v) { v ? p.enableFullScreen() : p.disableFullScreen(); });
		FlagCheckbox("Background", [&] { return p.isBackgroundEnabled(); },
			[&](bool v) { v ? p.enableBackground() : p.disableBackground(); });
		FlagCheckbox("Depth test", [&] { return p.isDepthTestEnabled(); },
			[&](bool v) { v ? p.enableDepthTest() : p.disableDepthTest(); });
		FlagCheckbox("Corner rounding", [&] { return p.isCornerRoundingEnabled(); },
			[&](bool v) { v ? p.enableCornerRounding() : p.disableCornerRounding(); });
		FlagCheckbox("Extract normal map", [&] { return p.isExtractNormalMapEnabled(); },
			[&](bool v) { v ? p.enableExtractNormalMap() : p.disableExtractNormalMap(); });
		FlagCheckbox("Distortion mask", [&] { return p.isDistortionMaskEnabled(); },
			[&](bool v) { v ? p.enableDistortionMask() : p.disableDistortionMask(); });
		FlagCheckbox("Highlight", [&] { return p.isHighlightEnabled(); },
			[&](bool v) { v ? p.enableHighlight() : p.disableHighlight(); });
		if (p.isHighlightEnabled())
		{
			ImGui::ColorEdit3("Highlight color", &p.highlight_color.x);
			track("Set Highlight Color");
			ImGui::DragFloat("Highlight spread", &p.highlight_spread, 0.01f, 0.0f, 100.0f);
			track("Set Highlight Spread");
			ImGui::DragFloat2("Highlight position", &p.highlight_pos.x, 0.005f);
			track("Set Highlight Position");
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Gradient"))
	{
		static const char* gradients[] = { "None", "Linear", "Linear reflected", "Circular" };
		int g = (int)p.gradient;
		if (ImGui::Combo("Gradient", &g, gradients, IM_ARRAYSIZE(gradients)))
			p.gradient = (wi::image::Params::Gradient)g;
		track("Set Sprite Gradient");
		ImGui::DragFloat2("UV start", &p.gradient_uv_start.x, 0.005f);
		track("Set Gradient UV Start");
		ImGui::DragFloat2("UV end", &p.gradient_uv_end.x, 0.005f);
		track("Set Gradient UV End");
		ImGui::ColorEdit4("Gradient color", &p.gradient_color.x);
		track("Set Gradient Color");
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Animation"))
	{
		ImGui::Checkbox("Repeatable", &s->anim.repeatable);
		track("Set Sprite Anim Repeatable");
		ImGui::DragFloat3("Velocity", &s->anim.vel.x, 0.01f);
		track("Set Sprite Anim Velocity");
		ImGui::DragFloat("Rotation speed", &s->anim.rot, 0.005f);
		track("Set Sprite Anim Rotation");
		ImGui::DragFloat("Scale X speed", &s->anim.scaleX, 0.005f);
		track("Set Sprite Anim Scale X");
		ImGui::DragFloat("Scale Y speed", &s->anim.scaleY, 0.005f);
		track("Set Sprite Anim Scale Y");
		ImGui::DragFloat("Opacity speed", &s->anim.opa, 0.005f);
		track("Set Sprite Anim Opacity");
		ImGui::DragFloat("Fade speed", &s->anim.fad, 0.005f);
		track("Set Sprite Anim Fade");
		ImGui::DragFloat2("Texture scroll", &s->anim.movingTexAnim.speedX, 0.005f);
		track("Set Sprite Tex Scroll");

		ImGui::SeparatorText("Sprite sheet");
		ImGui::DragFloat("Frame rate", &s->anim.drawRectAnim.frameRate, 0.5f, 0.0f, 240.0f);
		track("Set Sprite Frame Rate");
		ImGui::DragInt("Frame count", &s->anim.drawRectAnim.frameCount, 0.2f, 1, 4096);
		track("Set Sprite Frame Count");
		ImGui::DragInt("Horizontal frames", &s->anim.drawRectAnim.horizontalFrameCount, 0.2f, 0, 4096);
		track("Set Sprite Horizontal Frames");
		ImGui::Text("Current frame: %d", s->anim.drawRectAnim._currentFrame);

		ImGui::SeparatorText("Wobble");
		ImGui::DragFloat2("Amount", &s->anim.wobbleAnim.amount.x, 0.005f);
		track("Set Sprite Wobble Amount");
		ImGui::DragFloat("Speed", &s->anim.wobbleAnim.speed, 0.01f);
		track("Set Sprite Wobble Speed");
		ImGui::TreePop();
	}
}

// sprite font

void DrawSpriteFont(Scene& scene, Entity e, st::EditorHistory* history)
{
	wi::SpriteFont* f = scene.fonts.GetComponent(e);
	if (!f) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::fonts", "Sprite Font", false, history)) return;
	const Edits track{ scene, e, history };

	// The component stores wide text; SetText/GetTextA are the conversion seam, so the editor
	//	round-trips through them rather than poking the wstring.
	std::string text = f->GetTextA();
	if (EditString("Text", text))
		f->SetText(text);
	track("Set Text");

	AssetDropField("Font style", f->fontStyleName);
	track("Set Font Style");
	HelpMarker("A .ttf resolved at load. Empty uses the engine default face.");

	FlagCheckbox("Hidden", [&] { return f->IsHidden(); }, [&](bool v) { f->SetHidden(v); });
	FlagCheckbox("Disable update", [&] { return f->IsDisableUpdate(); },
		[&](bool v) { f->SetDisableUpdate(v); });
	FlagCheckbox("Camera facing", [&] { return f->IsCameraFacing(); },
		[&](bool v) { f->SetCameraFacing(v); });
	FlagCheckbox("Camera scaling", [&] { return f->IsCameraScaling(); },
		[&](bool v) { f->SetCameraScaling(v); });

	wi::font::Params& p = f->params;
	if (ImGui::TreeNodeEx("Layout", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::DragFloat3("Position", &p.position.x, 0.5f);
		track("Set Font Position");
		ImGui::DragInt("Size", &p.size, 0.2f, 1, 512);
		track("Set Font Size");
		ImGui::DragFloat("Scaling", &p.scaling, 0.01f, 0.01f, 100.0f);
		track("Set Font Scaling");
		DragAngle("Rotation", p.rotation);
		track("Set Font Rotation");
		ImGui::DragFloat("Spacing X", &p.spacingX, 0.05f);
		track("Set Font Spacing X");
		ImGui::DragFloat("Spacing Y", &p.spacingY, 0.05f);
		track("Set Font Spacing Y");
		ImGui::DragFloat("Wrap width", &p.h_wrap, 0.5f, -1.0f, 100000.0f);
		track("Set Font Wrap");
		HelpMarker("-1 disables wrapping.");

		static const char* hAlign[] = { "Left", "Center", "Right" };
		int h = (int)p.h_align;
		if (h <= 2 && ImGui::Combo("Horizontal align", &h, hAlign, IM_ARRAYSIZE(hAlign)))
			p.h_align = (wi::font::Alignment)h;
		track("Set Font H Align");

		// Vertical alignment reuses WIFALIGN_CENTER, so the enum values are 3 (top),
		//	1 (center) and 4 (bottom) rather than a contiguous run.
		static const int vAlignValues[] = { wi::font::WIFALIGN_TOP, wi::font::WIFALIGN_CENTER,
			wi::font::WIFALIGN_BOTTOM };
		static const char* vAlign[] = { "Top", "Center", "Bottom" };
		int v = 0;
		for (int i = 0; i < 3; ++i) if ((int)p.v_align == vAlignValues[i]) v = i;
		if (ImGui::Combo("Vertical align", &v, vAlign, IM_ARRAYSIZE(vAlign)))
			p.v_align = (wi::font::Alignment)vAlignValues[v];
		track("Set Font V Align");
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Appearance", ImGuiTreeNodeFlags_DefaultOpen))
	{
		XMFLOAT4 color = p.color.toFloat4();
		if (ImGui::ColorEdit4("Color", &color.x)) p.color = wi::Color::fromFloat4(color);
		track("Set Font Color");
		XMFLOAT4 shadow = p.shadowColor.toFloat4();
		if (ImGui::ColorEdit4("Shadow color", &shadow.x)) p.shadowColor = wi::Color::fromFloat4(shadow);
		track("Set Font Shadow Color");
		HelpMarker("A fully transparent shadow colour turns the shadow off.");
		ImGui::DragFloat("Intensity", &p.intensity, 0.01f, 0.0f, 100.0f);
		track("Set Font Intensity");
		ImGui::DragFloat("Shadow intensity", &p.shadow_intensity, 0.01f, 0.0f, 100.0f);
		track("Set Font Shadow Intensity");
		ImGui::DragFloat("Shadow offset X", &p.shadow_offset_x, 0.05f);
		track("Set Font Shadow Offset X");
		ImGui::DragFloat("Shadow offset Y", &p.shadow_offset_y, 0.05f);
		track("Set Font Shadow Offset Y");
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("SDF"))
	{
		FlagCheckbox("SDF rendering", [&] { return p.isSDFRenderingEnabled(); },
			[&](bool v) { v ? p.enableSDFRendering() : p.disableSDFRendering(); });
		ImGui::BeginDisabled(!p.isSDFRenderingEnabled());
		ImGui::DragFloat("Softness", &p.softness, 0.005f, 0.0f, 1.0f);
		track("Set Font Softness");
		ImGui::DragFloat("Bolden", &p.bolden, 0.005f, 0.0f, 1.0f);
		track("Set Font Bolden");
		ImGui::DragFloat("Shadow softness", &p.shadow_softness, 0.005f, 0.0f, 1.0f);
		track("Set Font Shadow Softness");
		ImGui::DragFloat("Shadow bolden", &p.shadow_bolden, 0.005f, 0.0f, 1.0f);
		track("Set Font Shadow Bolden");
		ImGui::EndDisabled();

		FlagCheckbox("Depth test", [&] { return p.isDepthTestEnabled(); },
			[&](bool v) { v ? p.enableDepthTest() : p.disableDepthTest(); });
		FlagCheckbox("Flip horizontally", [&] { return p.isFlippedHorizontally(); },
			[&](bool v) { v ? p.enableFlipHorizontally() : p.disableFlipHorizontally(); });
		FlagCheckbox("Flip vertically", [&] { return p.isFlippedVertically(); },
			[&](bool v) { v ? p.enableFlipVertically() : p.disableFlipVertically(); });
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Typewriter"))
	{
		ImGui::DragFloat("Duration (s)", &f->anim.typewriter.time, 0.01f, 0.0f, 60.0f);
		track("Set Typewriter Time");
		HelpMarker("0 disables the effect.");
		ImGui::Checkbox("Looped", &f->anim.typewriter.looped);
		track("Set Typewriter Looped");
		int start = (int)f->anim.typewriter.character_start;
		if (ImGui::DragInt("Start character", &start, 0.2f, 0, 4096))
			f->anim.typewriter.character_start = (size_t)(start < 0 ? 0 : start);
		track("Set Typewriter Start");
		if (ImGui::Button("Restart")) f->anim.typewriter.reset();
		ImGui::SameLine();
		if (ImGui::Button("Finish")) f->anim.typewriter.Finish();
		ImGui::TreePop();
	}

	const XMFLOAT2 size = f->TextSize();
	ImGui::Text("Measured: %.1f x %.1f", size.x, size.y);
}

// voxel grid

void DrawVoxelGrid(Scene& scene, Entity e, st::EditorHistory* history)
{
	wi::VoxelGrid* g = scene.voxel_grids.GetComponent(e);
	if (!g) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::voxel_grids", "Voxel Grid", false, history)) return;
	const Edits track{ scene, e, history };

	// init() reallocates the bitfield, so resolution goes through it rather than a raw store
	//	and it is an InputInt3 rather than a drag on purpose: the allocation is the product
	//	of the three axes, so dragging one of them through 512 would allocate a gigabyte on
	//	the way to wherever the cursor was heading.
	int res[3] = { (int)g->resolution.x, (int)g->resolution.y, (int)g->resolution.z };
	if (ImGui::InputInt3("Resolution", res, ImGuiInputTextFlags_EnterReturnsTrue))
	{
		auto clampDim = [](int v) { return (uint32_t)(v < 0 ? 0 : (v > 512 ? 512 : v)); };
		g->init(clampDim(res[0]), clampDim(res[1]), clampDim(res[2]));
	}
	track("Set Voxel Resolution");

	ImGui::DragFloat3("Center", &g->center.x, 0.05f);
	track("Set Voxel Center");

	XMFLOAT3 voxelSize = g->voxelSize;
	if (ImGui::DragFloat3("Voxel size", &voxelSize.x, 0.005f, 0.0001f, 1000.0f))
		g->set_voxelsize(voxelSize);   // also refreshes the reciprocal the lookups use
	track("Set Voxel Size");

	ImGui::ColorEdit4("Debug color", &g->debug_color.x);
	track("Set Voxel Debug Color");
	ImGui::ColorEdit4("Debug extent color", &g->debug_color_extent.x);
	track("Set Voxel Extent Color");

	ImGui::Text("Memory: %zu bytes", g->get_memory_size());
	if (ImGui::Button("Clear voxels"))
	{
		if (history) history->BeginEntity(scene, e, "Clear Voxel Grid");
		g->cleardata();
		if (history) history->Commit(scene);
	}
	ImGui::TextDisabled("Occupancy is filled by the navigation/pathfinding pass, not by hand.");
}

// metadata

void DrawMetadata(Scene& scene, Entity e, st::EditorHistory* history)
{
	MetadataComponent* md = scene.metadatas.GetComponent(e);
	if (!md) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::metadatas", "Metadata", false, history)) return;
	const Edits track{ scene, e, history };

	static const char* presets[] = { "Custom", "Waypoint", "Player", "Enemy", "NPC", "Pickup", "Vehicle" };
	int preset = (int)md->preset;
	if (ImGui::Combo("Preset", &preset, presets, IM_ARRAYSIZE(presets)))
		md->preset = (MetadataComponent::Preset)preset;
	track("Set Metadata Preset");

	ImGui::TextDisabled("Native-component attachments (NCI_/NCA_/NCE_) live in the string and "
		"int tables below. Renaming or deleting one of those detaches the component.");

	// Deleting from an OrderedNamedValues reindexes it, so the erase is deferred to after the
	//	loop that is walking it.
	std::string eraseBool, eraseInt, eraseFloat, eraseString;

	auto removeButton = [&](const std::string& name, std::string& sink) {
		ImGui::SameLine();
		if (ImGui::SmallButton("x")) sink = name;
	};

	if (ImGui::TreeNodeEx("Bool", ImGuiTreeNodeFlags_DefaultOpen))
	{
		for (size_t i = 0; i < md->bool_values.names.size(); ++i)
		{
			ImGui::PushID((int)i);
			bool v = md->bool_values.values[i];
			if (ImGui::Checkbox(md->bool_values.names[i].c_str(), &v))
				md->bool_values.values[i] = v;
			track("Set Metadata Bool");
			removeButton(md->bool_values.names[i], eraseBool);
			ImGui::PopID();
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Int", ImGuiTreeNodeFlags_DefaultOpen))
	{
		for (size_t i = 0; i < md->int_values.names.size(); ++i)
		{
			ImGui::PushID((int)i);
			int v = md->int_values.values[i];
			if (ImGui::InputInt(md->int_values.names[i].c_str(), &v))
				md->int_values.values[i] = v;
			track("Set Metadata Int");
			removeButton(md->int_values.names[i], eraseInt);
			ImGui::PopID();
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Float", ImGuiTreeNodeFlags_DefaultOpen))
	{
		for (size_t i = 0; i < md->float_values.names.size(); ++i)
		{
			ImGui::PushID((int)i);
			float v = md->float_values.values[i];
			if (ImGui::DragFloat(md->float_values.names[i].c_str(), &v, 0.01f))
				md->float_values.values[i] = v;
			track("Set Metadata Float");
			removeButton(md->float_values.names[i], eraseFloat);
			ImGui::PopID();
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("String", ImGuiTreeNodeFlags_DefaultOpen))
	{
		for (size_t i = 0; i < md->string_values.names.size(); ++i)
		{
			ImGui::PushID((int)i);
			EditString(md->string_values.names[i].c_str(), md->string_values.values[i]);
			track("Set Metadata String");
			removeButton(md->string_values.names[i], eraseString);
			ImGui::PopID();
		}
		ImGui::TreePop();
	}

	// add a new entry
	static char newName[64] = "";
	static int  newType = 0;
	ImGui::SetNextItemWidth(140);
	ImGui::InputTextWithHint("##md_name", "new key", newName, sizeof(newName));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(90);
	static const char* valueTypes[] = { "bool", "int", "float", "string" };
	ImGui::Combo("##md_type", &newType, valueTypes, IM_ARRAYSIZE(valueTypes));
	ImGui::SameLine();
	if (ImGui::Button("Add") && newName[0] != 0)
	{
		if (history) history->BeginEntity(scene, e, "Add Metadata Value");
		switch (newType)
		{
		case 0: md->bool_values.set(newName, false); break;
		case 1: md->int_values.set(newName, 0); break;
		case 2: md->float_values.set(newName, 0.0f); break;
		default: md->string_values.set(newName, std::string()); break;
		}
		if (history) history->Commit(scene);
		newName[0] = 0;
	}

	if (!eraseBool.empty() || !eraseInt.empty() || !eraseFloat.empty() || !eraseString.empty())
	{
		if (history) history->BeginEntity(scene, e, "Remove Metadata Value");
		if (!eraseBool.empty())   md->bool_values.erase(eraseBool);
		if (!eraseInt.empty())    md->int_values.erase(eraseInt);
		if (!eraseFloat.empty())  md->float_values.erase(eraseFloat);
		if (!eraseString.empty()) md->string_values.erase(eraseString);
		if (history) history->Commit(scene);
	}
}

// character

void DrawCharacter(Scene& scene, Entity e, st::EditorHistory* history)
{
	CharacterComponent* c = scene.characters.GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::characters", "Character", false, history)) return;
	const Edits track{ scene, e, history };

	ImGui::DragInt("Health", &c->health, 1.0f, 0, 100000);
	track("Set Health");
	ImGui::DragFloat("Capsule radius", &c->width, 0.005f, 0.01f, 10.0f);
	track("Set Character Radius");
	ImGui::DragFloat("Capsule height", &c->height, 0.01f, 0.01f, 10.0f);
	track("Set Character Height");
	ImGui::DragFloat("Scale", &c->scale, 0.01f, 0.01f, 100.0f);
	track("Set Character Scale");

	FlagCheckbox("Active", [&] { return c->IsActive(); }, [&](bool v) { c->SetActive(v); });
	FlagCheckbox("Disable character-to-character collision",
		[&] { return c->IsCharacterToCharacterCollisionDisabled(); },
		[&](bool v) { c->SetCharacterToCharacterCollisionDisabled(v); });
	FlagCheckbox("Dedicated shadow", [&] { return c->IsDedicatedShadow(); },
		[&](bool v) { c->SetDedicatedShadow(v); });
	FlagCheckbox("Foot placement (IK)", [&] { return c->IsFootPlacementEnabled(); },
		[&](bool v) { c->SetFootPlacementEnabled(v); });

	if (ImGui::TreeNodeEx("Movement", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::TextDisabled("Tunables below are not serialized: they are runtime settings the "
			"controller reads every step, so a scene save does not carry them.");
		ImGui::DragFloat("Gravity", &c->gravity, 0.1f, -1000.0f, 1000.0f);
		track("Set Character Gravity");
		ImGui::DragFloat("Ground friction", &c->ground_friction, 0.005f, 0.0f, 1.0f);
		track("Set Ground Friction");
		ImGui::DragFloat("Water friction", &c->water_friction, 0.005f, 0.0f, 1.0f);
		track("Set Water Friction");
		ImGui::DragFloat("Slope threshold", &c->slope_threshold, 0.005f, 0.0f, 1.0f);
		track("Set Slope Threshold");
		ImGui::DragFloat("Leaning limit", &c->leaning_limit, 0.005f, 0.0f, 1.0f);
		track("Set Leaning Limit");
		ImGui::DragFloat("Turning speed", &c->turning_speed, 0.05f, 0.0f, 100.0f);
		track("Set Turning Speed");
		ImGui::DragFloat("Fixed update FPS", &c->fixed_update_fps, 1.0f, 1.0f, 480.0f);
		track("Set Character Fixed FPS");
		ImGui::DragFloat("Foot offset", &c->foot_offset, 0.005f);
		track("Set Foot Offset");
		ImGui::DragFloat("Water vertical offset", &c->water_vertical_offset, 0.005f);
		track("Set Water Vertical Offset");
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Bindings"))
	{
		EntityRef(scene, "Humanoid", c->humanoidEntity);
		EntityRef(scene, "Left foot", c->left_foot);
		EntityRef(scene, "Right foot", c->right_foot);
		EntityRef(scene, "Current animation", c->currentAnimation);
		ImGui::Text("Registered animations: %zu", c->animations.size());
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("State"))
	{
		const XMFLOAT3 pos = c->GetPosition();
		const XMFLOAT3 vel = c->GetVelocity();
		ImGui::Text("Position: %.2f %.2f %.2f", pos.x, pos.y, pos.z);
		ImGui::Text("Velocity: %.2f %.2f %.2f", vel.x, vel.y, vel.z);
		ImGui::Text("Grounded: %s   Wall: %s   Swimming: %s",
			c->IsGrounded() ? "yes" : "no",
			c->IsWallIntersect() ? "yes" : "no",
			c->IsSwimming() ? "yes" : "no");
		ImGui::Text("Leaning: %.3f", c->GetLeaningSmoothed());
		ImGui::TreePop();
	}
}

// spline

void DrawSpline(Scene& scene, Entity e, st::EditorHistory* history)
{
	SplineComponent* s = scene.splines.GetComponent(e);
	if (!s) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::splines", "Spline", false, history)) return;
	const Edits track{ scene, e, history };

	// Every field here feeds mesh or terrain generation, and the spline system only redoes
	//	that work when the dirty flag is up.
	bool dirty = false;

	ImGui::Text("%zu nodes, total length %.2f",
		s->spline_node_entities.size(), s->precomputed_total_distance);
	ImGui::TextDisabled("Nodes are child entities; move one with its own Transform.");

	dirty |= ImGui::DragFloat("Width", &s->width, 0.01f, 0.0f, 1000.0f);
	track("Set Spline Width");
	dirty |= DragAngle("Rotation", s->rotation);
	track("Set Spline Rotation");

	dirty |= FlagCheckbox("Looped", [&] { return s->IsLooped(); }, [&](bool v) { s->SetLooped(v); });
	dirty |= FlagCheckbox("Draw aligned", [&] { return s->IsDrawAligned(); },
		[&](bool v) { s->SetDrawAligned(v); });
	HelpMarker("Off draws the ribbon camera-facing; on aligns it to the node rotations.");
	dirty |= FlagCheckbox("Filled", [&] { return s->IsFilled(); }, [&](bool v) { s->SetFilled(v); });

	if (ImGui::TreeNodeEx("Mesh generation", ImGuiTreeNodeFlags_DefaultOpen))
	{
		dirty |= ImGui::DragInt("Subdivision", &s->mesh_generation_subdivision, 0.1f, 0, 64);
		track("Set Spline Subdivision");
		HelpMarker("Above 0 makes the spline generate a mesh.");
		dirty |= ImGui::DragInt("Vertical subdivision", &s->mesh_generation_vertical_subdivision, 0.1f, 0, 64);
		track("Set Spline Vertical Subdivision");
		HelpMarker("Extrudes the ribbon into a corridor or tunnel.");

		static const char* normalModes[] = { "Hard", "Smooth", "Smooth (fast)" };
		int nm = (int)s->fill_normals_mode;
		if (nm >= 0 && nm < IM_ARRAYSIZE(normalModes) &&
			ImGui::Combo("Fill normals", &nm, normalModes, IM_ARRAYSIZE(normalModes)))
		{
			s->fill_normals_mode = (MeshComponent::COMPUTE_NORMALS)nm;
			dirty = true;
		}
		track("Set Spline Fill Normals");
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Terrain modifier"))
	{
		dirty |= ImGui::DragFloat("Amount", &s->terrain_modifier_amount, 0.01f, 0.0f, 100.0f);
		track("Set Terrain Modifier Amount");
		HelpMarker("Above 0 makes the terrain generator flatten towards the spline plane.");
		dirty |= ImGui::DragFloat("Pushdown", &s->terrain_pushdown, 0.01f, -1000.0f, 1000.0f);
		track("Set Terrain Pushdown");
		dirty |= ImGui::DragFloat("Texture falloff", &s->terrain_texture_falloff, 0.005f, 0.0f, 1.0f);
		track("Set Terrain Texture Falloff");
		ImGui::TreePop();
	}

	if (dirty)
		s->SetDirty();
}

// gaussian splat

void DrawGaussianSplat(Scene& scene, Entity e, st::EditorHistory* history)
{
	wi::GaussianSplatModel* g = scene.gaussian_splats.GetComponent(e);
	if (!g) return;
	if (!ComponentHeader(scene, e, "wi::scene::Scene::gaussian_splats", "Gaussian Splat", false, history)) return;

	ImGui::Text("%zu splats", g->GetSplatCount());
	ImGui::Text("Spherical harmonics degree: %d", g->GetSphericalHarmonicsDegree());
	ImGui::Text("CPU: %zu bytes, GPU: %zu bytes", g->GetMemorySizeCPU(), g->GetMemorySizeGPU());
	ImGui::Text("Max scale: %.4f", g->maxScale);
	ImGui::Text("AABB: %.2f %.2f %.2f .. %.2f %.2f %.2f",
		g->aabb._min.x, g->aabb._min.y, g->aabb._min.z,
		g->aabb._max.x, g->aabb._max.y, g->aabb._max.z);
	ImGui::TextDisabled("A splat cloud is imported point data with no authorable parameters; "
		"place and scale it with the Transform on this entity.");

	if (ImGui::Button("Rebuild GPU buffers"))
		g->CreateRenderData();
}

// dispatch

// One row per manager, in the same order the Add Component catalogue lists them, so the
//	inspector and that menu read alike. Names and transforms are absent on purpose - see the
//	header.
struct InspectorEntry
{
	const char* libraryKey;
	void (*draw)(Scene&, Entity, st::EditorHistory*);
};

const InspectorEntry g_inspectors[] = {
	{ "wi::scene::Scene::layers",             DrawLayer },
	{ "wi::scene::Scene::hierarchy",          DrawHierarchy },
	{ "wi::scene::Scene::materials",          DrawMaterial },
	{ "wi::scene::Scene::meshes",             DrawMesh },
	{ "wi::scene::Scene::impostors",          DrawImpostor },
	{ "wi::scene::Scene::objects",            DrawObject },
	{ "wi::scene::Scene::rigidbodies",        DrawRigidBody },
	{ "wi::scene::Scene::softbodies",         DrawSoftBody },
	{ "wi::scene::Scene::armatures",          DrawArmature },
	{ "wi::scene::Scene::lights",             DrawLight },
	{ "wi::scene::Scene::cameras",            DrawCamera },
	{ "wi::scene::Scene::probes",             DrawProbe },
	{ "wi::scene::Scene::forces",             DrawForceField },
	{ "wi::scene::Scene::decals",             DrawDecal },
	{ "wi::scene::Scene::animations",         DrawAnimation },
	{ "wi::scene::Scene::animation_datas",    DrawAnimationData },
	{ "wi::scene::Scene::emitters",           DrawEmitter },
	{ "wi::scene::Scene::hairs",              DrawHair },
	{ "wi::scene::Scene::weathers",           DrawWeather },
	{ "wi::scene::Scene::sounds",             DrawSound },
	{ "wi::scene::Scene::videos",             DrawVideo },
	{ "wi::scene::Scene::inverse_kinematics", DrawIK },
	{ "wi::scene::Scene::springs",            DrawSpring },
	{ "wi::scene::Scene::colliders",          DrawCollider },
	{ "wi::scene::Scene::scripts",            DrawScript },
	{ "wi::scene::Scene::expressions",        DrawExpression },
	{ "wi::scene::Scene::humanoids",          DrawHumanoid },
	{ "wi::scene::Scene::terrains",           DrawTerrain },
	{ "wi::scene::Scene::sprites",            DrawSprite },
	{ "wi::scene::Scene::fonts",              DrawSpriteFont },
	{ "wi::scene::Scene::voxel_grids",        DrawVoxelGrid },
	{ "wi::scene::Scene::metadatas",          DrawMetadata },
	{ "wi::scene::Scene::characters",         DrawCharacter },
	{ "wi::scene::Scene::constraints",        DrawConstraint },
	{ "wi::scene::Scene::splines",            DrawSpline },
	{ "wi::scene::Scene::gaussian_splats",    DrawGaussianSplat },
};

} // namespace

void EngineComponentInspectors(Scene& scene, Entity entity, st::EditorHistory* history)
{
	if (entity == INVALID_ENTITY) return;

	// Each editor pushes its own ID scope, so two components whose fields happen to share a
	//	label ("Radius" on a collider and on a rigid body) do not collide in ImGui state.
	for (const InspectorEntry& entry : g_inspectors)
	{
		ImGui::PushID(entry.libraryKey);
		entry.draw(scene, entity, history);
		ImGui::PopID();
	}
}

bool HasEngineComponentInspector(const std::string& libraryKey)
{
	// Drawn by the Properties panel itself rather than from the table above.
	if (libraryKey == "wi::scene::Scene::names") return true;
	if (libraryKey == "wi::scene::Scene::transforms") return true;

	for (const InspectorEntry& entry : g_inspectors)
		if (libraryKey == entry.libraryKey) return true;
	return false;
}

} // namespace st::devui
