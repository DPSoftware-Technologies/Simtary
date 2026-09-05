// FBX and Wavefront OBJ, through ufbx.
//
//	One backend for two formats because ufbx reads both, and reads them into the same
//	normalised scene graph: by the time this file sees a model it no longer matters whether
//	the exporter was Maya, Blender or a text editor.
//
//	What ufbx is asked to do before we look at anything
//
//	  target_axes / target_unit_meters   convert into the engine's left-handed Y-up metres,
//	                                     geometry and transforms together, so nothing here
//	                                     has to mirror or rescale by hand
//	  generate_missing_normals           an OBJ without normals is common and renders black
//	  open_file_cb                       every side file (an .mtl, a texture) is fetched with
//	                                     wi::helper::FileRead, so a model inside an asset
//	                                     package finds its own dependencies
//
//	Skinning
//
//	FBX stores a skin as clusters, each naming one bone and the matrix that takes geometry
//	into that bone's space - which is the inverse bind matrix the engine wants, so the two
//	line up directly. Weights arrive sorted by influence, so taking the first four and
//	renormalising is a principled truncation rather than an arbitrary one.
//
//	Animation
//
//	Baked with ufbx_bake_anim rather than read as curves. FBX animation is not a set of TRS
//	curves: it is curves plus pre/post-rotation, rotation order, pivots and limits, and
//	evaluating that correctly is most of what an FBX loader is. Baking asks ufbx to do it and
//	hands back plain linear keys, which is exactly the shape AnimationComponent stores.

#include "ModelImporterCommon.h"

#include "wiHelper.h"
#include "wiBacklog.h"
#include "wiMath.h"

#include <ufbx.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using wi::ecs::Entity;
using wi::ecs::INVALID_ENTITY;
using namespace wi::scene;

namespace st::model::detail {

namespace {

std::string Str (const ufbx_string& s)
{
	return (s.data != nullptr && s.length > 0) ? std::string(s.data, s.length) : std::string();
}

XMFLOAT3 Vec3 (const ufbx_vec3& v)
{
	return XMFLOAT3((float)v.x, (float)v.y, (float)v.z);
}

XMFLOAT4 Quat (const ufbx_quat& q)
{
	return XMFLOAT4((float)q.x, (float)q.y, (float)q.z, (float)q.w);
}

XMFLOAT4X4 Matrix (const ufbx_matrix& m)
{
	// ufbx_matrix is 3 rows of basis vectors plus a translation column, in column-major
	//	order; XMFLOAT4X4 is row-major with translation in the last ROW.
	XMFLOAT4X4 out;
	out._11 = (float)m.cols[0].x; out._12 = (float)m.cols[0].y; out._13 = (float)m.cols[0].z; out._14 = 0.0f;
	out._21 = (float)m.cols[1].x; out._22 = (float)m.cols[1].y; out._23 = (float)m.cols[1].z; out._24 = 0.0f;
	out._31 = (float)m.cols[2].x; out._32 = (float)m.cols[2].y; out._33 = (float)m.cols[2].z; out._34 = 0.0f;
	out._41 = (float)m.cols[3].x; out._42 = (float)m.cols[3].y; out._43 = (float)m.cols[3].z; out._44 = 1.0f;
	return out;
}

// file access
// ufbx opens side files through this and nothing else (the build defines UFBX_NO_STDIO), so
//	an .mtl or a texture is fetched exactly the way the engine fetches everything: mounted
//	asset packages first, real files second.

struct StreamData {
	std::vector<uint8_t> bytes;
	size_t               offset = 0;
};

size_t StreamRead (void* user, void* data, size_t size)
{
	StreamData* s = (StreamData*)user;
	const size_t remaining = s->bytes.size() - s->offset;
	const size_t n = size < remaining ? size : remaining;
	if (n > 0) std::memcpy(data, s->bytes.data() + s->offset, n);
	s->offset += n;
	return n;
}

bool StreamSkip (void* user, size_t size)
{
	StreamData* s = (StreamData*)user;
	if (s->offset + size > s->bytes.size()) return false;
	s->offset += size;
	return true;
}

void StreamClose (void* user)
{
	delete (StreamData*)user;
}

bool OpenFile (void* user, ufbx_stream* stream, const char* path, size_t path_len,
	const ufbx_open_file_info* info)
{
	(void)info;
	ImportContext* ctx = (ImportContext*)user;
	const std::string requested(path, path_len);

	// Same search order as a texture reference, and for the same reason: an exporter writes
	//	the path its own machine had.
	std::string resolved = requested;
	if (!wi::helper::FileExists(resolved) && ctx != nullptr)
	{
		resolved = ResolveTexturePath(*ctx, requested);
		if (resolved.empty()) return false;
	}

	StreamData* data = new StreamData();
	if (!ReadFile(resolved, data->bytes))
	{
		delete data;
		return false;
	}

	stream->read_fn  = &StreamRead;
	stream->skip_fn  = &StreamSkip;
	stream->close_fn = &StreamClose;
	stream->user     = data;
	return true;
}

// materials

// A texture reference becomes a name the engine can load: embedded content is registered
//	with the resource manager, a file name is resolved against the model's folder.
std::string TextureName (ImportContext& ctx, const ufbx_texture* texture)
{
	if (texture == nullptr) return std::string();

	if (texture->content.size > 0)
	{
		return RegisterEmbeddedTexture(ctx, (const uint8_t*)texture->content.data,
			texture->content.size, Str(texture->name));
	}

	// relative_filename is what the exporter meant; filename is ufbx's resolution of it
	//	against the model. Try the resolved one first.
	std::string name = ResolveTexturePath(ctx, Str(texture->filename));
	if (name.empty()) name = ResolveTexturePath(ctx, Str(texture->relative_filename));
	if (name.empty()) name = ResolveTexturePath(ctx, Str(texture->absolute_filename));
	if (!name.empty()) ctx.result.textures++;
	return name;
}

void SetSlot (ImportContext& ctx, MaterialComponent& material,
	MaterialComponent::TEXTURESLOT slot, const ufbx_material_map& map)
{
	if (!map.texture_enabled || map.texture == nullptr) return;
	const std::string name = TextureName(ctx, map.texture);
	if (!name.empty())
		material.textures[slot].name = name;
}

Entity ConvertMaterial (ImportContext& ctx, const ufbx_material* src)
{
	Scene& scene = *ctx.scene;
	const Entity e = CreateMaterial(ctx, Str(src->name).empty() ? (ctx.stem + "_material")
	                                                            : Str(src->name));
	MaterialComponent* m = scene.materials.GetComponent(e);
	if (m == nullptr) return e;

	// ufbx normalises every shading model it understands into `pbr`, filling it from the
	//	FBX legacy properties when that is all the file had. So this reads pbr and does not
	//	branch on whether the source was a Phong material or a modern PBR one.
	const ufbx_material_pbr_maps& pbr = src->pbr;

	if (pbr.base_color.has_value)
	{
		m->baseColor = XMFLOAT4(
			(float)pbr.base_color.value_vec4.x,
			(float)pbr.base_color.value_vec4.y,
			(float)pbr.base_color.value_vec4.z,
			pbr.base_color.value_components >= 4 ? (float)pbr.base_color.value_vec4.w : 1.0f);
	}
	if (pbr.opacity.has_value)
		m->baseColor.w = (float)pbr.opacity.value_real;
	if (pbr.roughness.has_value)
		m->roughness = (float)pbr.roughness.value_real;
	if (pbr.metalness.has_value)
		m->metalness = (float)pbr.metalness.value_real;
	if (pbr.emission_color.has_value)
	{
		const float strength = pbr.emission_factor.has_value
			? (float)pbr.emission_factor.value_real : 1.0f;
		m->emissiveColor = XMFLOAT4(
			(float)pbr.emission_color.value_vec3.x,
			(float)pbr.emission_color.value_vec3.y,
			(float)pbr.emission_color.value_vec3.z,
			strength);
	}

	SetSlot(ctx, *m, MaterialComponent::BASECOLORMAP,  pbr.base_color);
	SetSlot(ctx, *m, MaterialComponent::NORMALMAP,     pbr.normal_map);
	SetSlot(ctx, *m, MaterialComponent::EMISSIVEMAP,   pbr.emission_color);
	SetSlot(ctx, *m, MaterialComponent::TRANSPARENCYMAP, pbr.opacity);
	// The engine packs roughness and metalness into one surface map, which is the same
	//	layout glTF's metallicRoughness uses. Either source map is the better guess than none.
	if (pbr.roughness.texture_enabled && pbr.roughness.texture != nullptr)
		SetSlot(ctx, *m, MaterialComponent::SURFACEMAP, pbr.roughness);
	else
		SetSlot(ctx, *m, MaterialComponent::SURFACEMAP, pbr.metalness);

	if (src->features.double_sided.enabled)
		m->SetDoubleSided(true);

	m->SetDirty();
	m->CreateRenderData();
	return e;
}

// meshes

struct BoneMapping {
	std::unordered_map<const ufbx_node*, uint32_t> boneIndex; // node -> index in boneCollection
	Entity armature = INVALID_ENTITY;
};

// Build one MeshComponent from one ufbx_mesh. Each material part becomes a subset, which is
//	how the engine draws a multi-material mesh in one object.
Entity ConvertMesh (ImportContext& ctx, const ufbx_mesh* src,
	const std::vector<Entity>& materialEntities, const BoneMapping* skin)
{
	Scene& scene = *ctx.scene;
	const Entity meshEntity = scene.Entity_CreateMesh(
		UniqueName(scene, Str(src->name).empty() ? (ctx.stem + "_mesh") : Str(src->name)));
	MeshComponent* mesh = scene.meshes.GetComponent(meshEntity);
	if (mesh == nullptr) return meshEntity;

	const bool hasNormals  = src->vertex_normal.exists;
	const bool hasUV       = src->vertex_uv.exists;
	const bool hasUV1      = src->uv_sets.count > 1;
	const bool hasTangents = src->vertex_tangent.exists;
	const bool hasColors   = src->vertex_color.exists;
	const bool skinned     = (skin != nullptr && !src->skin_deformers.count == 0);

	const ufbx_skin_deformer* deformer =
		(skin != nullptr && src->skin_deformers.count > 0) ? src->skin_deformers.data[0] : nullptr;

	// Triangulated on the way in: the engine draws triangles, and a face in an FBX can be an
	//	n-gon. ufbx_triangulate_face wants room for the worst case, which max_face_triangles
	//	gives exactly.
	std::vector<uint32_t> triIndices;
	triIndices.resize(src->max_face_triangles * 3 + 3);

	// One vertex per (face-corner) index rather than per logical vertex: normals and UVs in
	//	these formats are per-corner, so sharing by position would weld across a hard edge or
	//	a UV seam and lose both.
	std::unordered_map<uint32_t, uint32_t> emitted;  // ufbx index -> our vertex index
	emitted.reserve(src->num_indices);

	auto emitVertex = [&](uint32_t ufbxIndex) -> uint32_t {
		auto it = emitted.find(ufbxIndex);
		if (it != emitted.end()) return it->second;

		const uint32_t out = (uint32_t)mesh->vertex_positions.size();
		mesh->vertex_positions.push_back(Vec3(ufbx_get_vertex_vec3(&src->vertex_position, ufbxIndex)));
		if (hasNormals)
			mesh->vertex_normals.push_back(Vec3(ufbx_get_vertex_vec3(&src->vertex_normal, ufbxIndex)));
		if (hasTangents)
		{
			const ufbx_vec3 t = ufbx_get_vertex_vec3(&src->vertex_tangent, ufbxIndex);
			mesh->vertex_tangents.push_back(XMFLOAT4((float)t.x, (float)t.y, (float)t.z, 1.0f));
		}
		if (hasUV)
		{
			const ufbx_vec2 uv = ufbx_get_vertex_vec2(&src->vertex_uv, ufbxIndex);
			// UV origin differs: these formats put (0,0) at the bottom-left, the engine at
			//	the top-left, so V is flipped once here rather than in every shader.
			mesh->vertex_uvset_0.push_back(XMFLOAT2((float)uv.x, 1.0f - (float)uv.y));
		}
		if (hasUV1)
		{
			const ufbx_vec2 uv = ufbx_get_vertex_vec2(&src->uv_sets.data[1].vertex_uv, ufbxIndex);
			mesh->vertex_uvset_1.push_back(XMFLOAT2((float)uv.x, 1.0f - (float)uv.y));
		}
		if (hasColors)
		{
			const ufbx_vec4 c = ufbx_get_vertex_vec4(&src->vertex_color, ufbxIndex);
			mesh->vertex_colors.push_back(wi::Color::fromFloat4(
				XMFLOAT4((float)c.x, (float)c.y, (float)c.z, (float)c.w)).rgba);
		}

		if (deformer != nullptr && skin != nullptr)
		{
			// Weights are per LOGICAL vertex, not per corner.
			const uint32_t vertex = src->vertex_indices.data[ufbxIndex];
			XMUINT4 indices = XMUINT4(0, 0, 0, 0);
			XMFLOAT4 weights = XMFLOAT4(0, 0, 0, 0);
			if (vertex < deformer->vertices.count)
			{
				const ufbx_skin_vertex& sv = deformer->vertices.data[vertex];
				// Sorted by decreasing influence, so the first four are the four that matter.
				const uint32_t take = sv.num_weights < 4 ? (uint32_t)sv.num_weights : 4u;
				float total = 0.0f;
				uint32_t idx[4] = { 0, 0, 0, 0 };
				float    wgt[4] = { 0, 0, 0, 0 };
				for (uint32_t i = 0; i < take; ++i)
				{
					const ufbx_skin_weight& sw = deformer->weights.data[sv.weight_begin + i];
					const ufbx_skin_cluster* cluster = deformer->clusters.data[sw.cluster_index];
					auto boneIt = skin->boneIndex.find(cluster->bone_node);
					if (boneIt == skin->boneIndex.end()) continue;
					idx[i] = boneIt->second;
					wgt[i] = (float)sw.weight;
					total += wgt[i];
				}
				// Truncating to four influences loses weight; renormalising keeps the vertex
				//	fully attached instead of shrinking towards the origin.
				if (total > 0.0f)
					for (float& w : wgt) w /= total;
				indices = XMUINT4(idx[0], idx[1], idx[2], idx[3]);
				weights = XMFLOAT4(wgt[0], wgt[1], wgt[2], wgt[3]);
			}
			mesh->vertex_boneindices.push_back(indices);
			mesh->vertex_boneweights.push_back(weights);
		}

		emitted[ufbxIndex] = out;
		return out;
	};

	// One subset per material part, in part order, so subset N is material N.
	for (size_t partIndex = 0; partIndex < src->material_parts.count; ++partIndex)
	{
		const ufbx_mesh_part& part = src->material_parts.data[partIndex];
		if (part.num_triangles == 0) continue;

		MeshComponent::MeshSubset subset;
		subset.indexOffset = (uint32_t)mesh->indices.size();
		subset.materialID = (partIndex < materialEntities.size())
			? materialEntities[partIndex] : INVALID_ENTITY;

		for (size_t f = 0; f < part.face_indices.count; ++f)
		{
			const ufbx_face face = src->faces.data[part.face_indices.data[f]];
			const uint32_t triCount = ufbx_triangulate_face(triIndices.data(), triIndices.size(),
				src, face);
			for (uint32_t t = 0; t < triCount * 3; ++t)
				mesh->indices.push_back(emitVertex(triIndices[t]));
		}

		subset.indexCount = (uint32_t)mesh->indices.size() - subset.indexOffset;
		if (subset.indexCount > 0)
			mesh->subsets.push_back(subset);
	}

	if (skin != nullptr && deformer != nullptr)
		mesh->armatureID = skin->armature;

	// ufbx already mirrored the geometry into left-handed space AND fixed the winding while
	//	doing it, so nothing is flipped here.
	FinalizeMesh(ctx, meshEntity, false);
	return meshEntity;
}

// animation

// Append one baked channel as an AnimationDataComponent + sampler + channel.
void AddChannel (Scene& scene, AnimationComponent& anim, Entity target,
	AnimationComponent::AnimationChannel::Path path,
	const std::vector<float>& times, const std::vector<float>& values)
{
	if (times.empty()) return;

	const Entity dataEntity = wi::ecs::CreateEntity();
	AnimationDataComponent& data = scene.animation_datas.Create(dataEntity);
	data.keyframe_times.resize(times.size());
	std::copy(times.begin(), times.end(), data.keyframe_times.begin());
	data.keyframe_data.resize(values.size());
	std::copy(values.begin(), values.end(), data.keyframe_data.begin());

	AnimationComponent::AnimationSampler sampler;
	sampler.data = dataEntity;
	sampler.mode = AnimationComponent::AnimationSampler::LINEAR;  // baking produces linear keys
	anim.samplers.push_back(sampler);

	AnimationComponent::AnimationChannel channel;
	channel.target = target;
	channel.path = path;
	channel.samplerIndex = (int)anim.samplers.size() - 1;
	anim.channels.push_back(channel);
}

void ConvertAnimations (ImportContext& ctx, const ufbx_scene* fbx, Entity root,
	const std::unordered_map<const ufbx_node*, Entity>& nodeEntities)
{
	Scene& scene = *ctx.scene;

	for (size_t s = 0; s < fbx->anim_stacks.count; ++s)
	{
		const ufbx_anim_stack* stack = fbx->anim_stacks.data[s];

		ufbx_bake_opts bakeOpts = {};
		ufbx_error bakeError;
		ufbx_baked_anim* baked = ufbx_bake_anim(fbx, stack->anim, &bakeOpts, &bakeError);
		if (baked == nullptr)
			continue;

		const Entity animEntity = scene.Entity_CreateTransform(
			UniqueName(scene, Str(stack->name).empty() ? (ctx.stem + "_anim") : Str(stack->name)));
		scene.Component_Attach(animEntity, root);
		AnimationComponent& anim = scene.animations.Create(animEntity);
		anim.start = (float)baked->playback_time_begin;
		anim.end   = (float)baked->playback_time_end;
		anim.SetLooped(true);

		std::vector<float> times, values;
		for (size_t n = 0; n < baked->nodes.count; ++n)
		{
			const ufbx_baked_node& node = baked->nodes.data[n];
			if (node.typed_id >= fbx->nodes.count) continue;
			auto it = nodeEntities.find(fbx->nodes.data[node.typed_id]);
			if (it == nodeEntities.end()) continue;
			const Entity target = it->second;

			times.clear(); values.clear();
			for (size_t k = 0; k < node.translation_keys.count; ++k)
			{
				const ufbx_baked_vec3& key = node.translation_keys.data[k];
				times.push_back((float)key.time);
				values.push_back((float)key.value.x);
				values.push_back((float)key.value.y);
				values.push_back((float)key.value.z);
			}
			AddChannel(scene, anim, target,
				AnimationComponent::AnimationChannel::Path::TRANSLATION, times, values);

			times.clear(); values.clear();
			for (size_t k = 0; k < node.rotation_keys.count; ++k)
			{
				const ufbx_baked_quat& key = node.rotation_keys.data[k];
				times.push_back((float)key.time);
				values.push_back((float)key.value.x);
				values.push_back((float)key.value.y);
				values.push_back((float)key.value.z);
				values.push_back((float)key.value.w);
			}
			AddChannel(scene, anim, target,
				AnimationComponent::AnimationChannel::Path::ROTATION, times, values);

			times.clear(); values.clear();
			for (size_t k = 0; k < node.scale_keys.count; ++k)
			{
				const ufbx_baked_vec3& key = node.scale_keys.data[k];
				times.push_back((float)key.time);
				values.push_back((float)key.value.x);
				values.push_back((float)key.value.y);
				values.push_back((float)key.value.z);
			}
			AddChannel(scene, anim, target,
				AnimationComponent::AnimationChannel::Path::SCALE, times, values);
		}

		ufbx_free_baked_anim(baked);

		if (anim.channels.empty())
			scene.Entity_Remove(animEntity, true);   // a stack that animates nothing we imported
		else
			ctx.result.animations++;
	}
}

} // namespace

// the import

bool ImportUFBX (ImportContext& ctx, Entity root)
{
	Scene& scene = *ctx.scene;

	std::vector<uint8_t> bytes;
	if (!ReadFile(ctx.sourcePath, bytes))
	{
		ctx.result.error = "cannot read " + ctx.sourcePath;
		return false;
	}

	ufbx_load_opts opts = {};
	// Convert once, in the loader, rather than mirroring geometry and transforms by hand
	//	afterwards. MODIFY_GEOMETRY bakes the conversion into the vertices so no node is left
	//	carrying a negative scale, which is what breaks normals downstream.
	opts.target_axes             = ufbx_axes_left_handed_y_up;
	opts.target_unit_meters      = 1.0f;
	opts.space_conversion        = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
	opts.handedness_conversion_axis = UFBX_MIRROR_AXIS_Z;
	opts.generate_missing_normals = ctx.options.generateMissingNormals;
	opts.load_external_files      = true;
	opts.ignore_missing_external_files = true;   // a missing .mtl is a warning, not a failure
	opts.obj_search_mtl_by_filename = true;
	opts.filename                 = ufbx_string{ ctx.sourcePath.c_str(), ctx.sourcePath.size() };
	opts.open_file_cb.fn          = &OpenFile;
	opts.open_file_cb.user        = &ctx;
	if (!ctx.options.importAnimations)
		opts.ignore_animation = true;

	ufbx_error error;
	ufbx_scene* fbx = ufbx_load_memory(bytes.data(), bytes.size(), &opts, &error);
	if (fbx == nullptr)
	{
		char description[512];
		ufbx_format_error(description, sizeof(description), &error);
		ctx.result.error = std::string("ufbx: ") + description;
		return false;
	}

	// materials
	std::vector<Entity> materials(fbx->materials.count, INVALID_ENTITY);
	for (size_t i = 0; i < fbx->materials.count; ++i)
		materials[i] = ConvertMaterial(ctx, fbx->materials.data[i]);

	// nodes
	// Every node becomes an entity first, so a parent always exists before its children and
	//	a skin cluster can name a bone that has not been visited yet.
	std::unordered_map<const ufbx_node*, Entity> nodeEntities;
	for (size_t i = 0; i < fbx->nodes.count; ++i)
	{
		const ufbx_node* node = fbx->nodes.data[i];
		if (node->is_root) continue;

		const Entity e = scene.Entity_CreateTransform(
			UniqueName(scene, Str(node->name).empty() ? "node" : Str(node->name)));
		nodeEntities[node] = e;
		ctx.result.nodes++;

		TransformComponent* t = scene.transforms.GetComponent(e);
		if (t != nullptr)
		{
			const ufbx_transform& local = node->local_transform;
			t->translation_local = Vec3(local.translation);
			t->rotation_local    = Quat(local.rotation);
			t->scale_local       = Vec3(local.scale);
			t->SetDirty();
		}
	}

	// Parent them, root-relative when the FBX parent is the scene root.
	for (auto& kv : nodeEntities)
	{
		const ufbx_node* node = kv.first;
		Entity parent = root;
		if (node->parent != nullptr && !node->parent->is_root)
		{
			auto it = nodeEntities.find(node->parent);
			if (it != nodeEntities.end()) parent = it->second;
		}
		// child_already_in_local_space: the transform read above IS the local one, so
		//	Component_Attach must not re-derive it from a world matrix that does not exist yet.
		scene.Component_Attach(kv.second, parent, true);
	}

	// skins
	// One armature per skin deformer, built before meshes so a mesh can point at it.
	std::unordered_map<const ufbx_skin_deformer*, BoneMapping> skins;
	if (ctx.options.importSkins)
	{
		for (size_t i = 0; i < fbx->skin_deformers.count; ++i)
		{
			const ufbx_skin_deformer* deformer = fbx->skin_deformers.data[i];
			BoneMapping mapping;
			mapping.armature = scene.Entity_CreateTransform(UniqueName(scene, ctx.stem + "_armature"));
			scene.Component_Attach(mapping.armature, root, true);

			ArmatureComponent& armature = scene.armatures.Create(mapping.armature);
			for (size_t c = 0; c < deformer->clusters.count; ++c)
			{
				const ufbx_skin_cluster* cluster = deformer->clusters.data[c];
				if (cluster->bone_node == nullptr) continue;
				auto it = nodeEntities.find(cluster->bone_node);
				if (it == nodeEntities.end()) continue;

				mapping.boneIndex[cluster->bone_node] = (uint32_t)armature.boneCollection.size();
				armature.boneCollection.push_back(it->second);
				// geometry_to_bone IS the inverse bind matrix: it takes a vertex from mesh
				//	space into the bone's rest space, which is what skinning multiplies by.
				armature.inverseBindMatrices.push_back(Matrix(cluster->geometry_to_bone));
			}
			ctx.result.bones += (uint32_t)armature.boneCollection.size();
			skins[deformer] = std::move(mapping);
		}
	}

	// meshes
	// Converted once per ufbx_mesh and shared by every node instancing it, which is what
	//	keeps a scene of 500 identical crates at one mesh rather than 500.
	std::unordered_map<const ufbx_mesh*, Entity> meshEntities;
	for (auto& kv : nodeEntities)
	{
		const ufbx_node* node = kv.first;
		if (node->mesh == nullptr) continue;

		const ufbx_mesh* src = node->mesh;
		auto meshIt = meshEntities.find(src);
		if (meshIt == meshEntities.end())
		{
			// Per-mesh materials, in material_parts order.
			std::vector<Entity> meshMaterials;
			meshMaterials.reserve(src->materials.count);
			for (size_t m = 0; m < src->materials.count; ++m)
			{
				const ufbx_material* material = src->materials.data[m];
				Entity entity = INVALID_ENTITY;
				if (material != nullptr && material->typed_id < materials.size())
					entity = materials[material->typed_id];
				meshMaterials.push_back(entity);
			}

			const BoneMapping* mapping = nullptr;
			if (src->skin_deformers.count > 0)
			{
				auto skinIt = skins.find(src->skin_deformers.data[0]);
				if (skinIt != skins.end()) mapping = &skinIt->second;
			}

			meshIt = meshEntities.emplace(src, ConvertMesh(ctx, src, meshMaterials, mapping)).first;
		}

		ObjectComponent& object = scene.objects.Create(kv.second);
		object.meshID = meshIt->second;
	}

	// lights
	if (ctx.options.importLights)
	{
		for (auto& kv : nodeEntities)
		{
			const ufbx_light* light = kv.first->light;
			if (light == nullptr) continue;
			LightComponent& l = scene.lights.Create(kv.second);
			switch (light->type)
			{
			case UFBX_LIGHT_DIRECTIONAL: l.type = LightComponent::DIRECTIONAL; break;
			case UFBX_LIGHT_SPOT:        l.type = LightComponent::SPOT; break;
			default:                     l.type = LightComponent::POINT; break;
			}
			l.color     = Vec3(light->color);
			l.intensity = (float)light->intensity;
			l.SetCastShadow(true);
		}
	}

	// cameras
	if (ctx.options.importCameras)
	{
		for (auto& kv : nodeEntities)
		{
			const ufbx_camera* camera = kv.first->camera;
			if (camera == nullptr) continue;
			CameraComponent& c = scene.cameras.Create(kv.second);
			c.fov = (float)(camera->field_of_view_deg.y * (XM_PI / 180.0));
			c.zNearP = (float)camera->near_plane;
			c.zFarP  = (float)camera->far_plane;
			c.SetDirty();
		}
	}

	// animation
	if (ctx.options.importAnimations)
		ConvertAnimations(ctx, fbx, root, nodeEntities);

	ufbx_free_scene(fbx);
	return true;
}

} // namespace st::model::detail
