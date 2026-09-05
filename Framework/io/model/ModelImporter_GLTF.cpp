// glTF 2.0 and GLB, through tinygltf v3.
//
//	Handedness
//
//	glTF is right-handed, Y up, +Z toward the viewer. The engine is left-handed. Unlike the
//	FBX path - where ufbx converts during load - nothing here does it for us, so the mirror
//	is applied on the way in, in exactly three places and nowhere else:
//
//	  positions / normals / tangents   z = -z
//	  node translation                 z = -z, and the rotation quaternion becomes (-x,-y,z,w)
//	  triangles                        winding reversed, because mirroring one axis flips it
//
//	Doing it per-vertex rather than with a scale(1,1,-1) on the root matters: a negative scale
//	propagates into every child matrix, inverts face winding again at draw time and breaks
//	normal transformation. Baking it into the data leaves a scene with no mirrored transforms
//	in it at all.
//
//	Accessors
//
//	Everything geometric in glTF is an accessor over a buffer view: a component type, a count,
//	an optional stride, and an optional sparse override list. ReadAccessor normalises all of
//	that into floats (or uint32 for indices) once, so the mesh and animation code below never
//	sees a byteStride or a normalized short.
//
//	Files
//
//	tinygltf is given filesystem callbacks that go through wi::helper::FileRead, so a .gltf
//	with an external .bin and loose textures imports from inside an asset package. Image
//	decoding is left off (`images_as_is`): the bytes go to wi::resourcemanager, which is the
//	thing that knows how to make a GPU texture out of them.

#include "ModelImporterCommon.h"

#include "wiHelper.h"
#include "wiBacklog.h"
#include "wiMath.h"

#include <tiny_gltf_v3.h>

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using wi::ecs::Entity;
using wi::ecs::INVALID_ENTITY;
using namespace wi::scene;

namespace st::model::detail {

namespace {

std::string Str (const tg3_str& s)
{
	return (s.data != nullptr && s.len > 0) ? std::string(s.data, s.len) : std::string();
}

// filesystem callbacks

int32_t FsFileExists (const char* path, uint32_t path_len, void* user)
{
	(void)user;
	return wi::helper::FileExists(std::string(path, path_len)) ? 1 : 0;
}

int32_t FsReadFile (uint8_t** out_data, uint64_t* out_size, const char* path,
	uint32_t path_len, void* user)
{
	(void)user;
	std::vector<uint8_t> bytes;
	if (!ReadFile(std::string(path, path_len), bytes))
		return 0;
	// tinygltf takes ownership until it calls free_file, so this is a plain new[]/delete[]
	//	pair rather than a std::vector it cannot see the type of.
	uint8_t* copy = new uint8_t[bytes.size() > 0 ? bytes.size() : 1];
	if (!bytes.empty()) std::memcpy(copy, bytes.data(), bytes.size());
	*out_data = copy;
	*out_size = bytes.size();
	return 1;
}

void FsFreeFile (uint8_t* data, uint64_t size, void* user)
{
	(void)size; (void)user;
	delete[] data;
}

int32_t FsGetFileSize (uint64_t* out_size, const char* path, uint32_t path_len, void* user)
{
	(void)user;
	const size_t size = wi::helper::FileSize(std::string(path, path_len));
	if (size == 0) return 0;
	*out_size = size;
	return 1;
}

// accessors

// Where an accessor's raw bytes start, and how far apart consecutive elements are.
struct AccessorView {
	const uint8_t* base   = nullptr;
	size_t         stride = 0;
	size_t         count  = 0;
	int32_t        componentType = 0;
	int32_t        components    = 0;
	bool           normalized    = false;
	bool           valid () const { return base != nullptr && components > 0; }
};

AccessorView ViewOf (const tg3_model& model, int32_t accessorIndex)
{
	AccessorView view;
	if (accessorIndex < 0 || (uint32_t)accessorIndex >= model.accessors_count) return view;

	const tg3_accessor& accessor = model.accessors[accessorIndex];
	view.count         = (size_t)accessor.count;
	view.componentType = accessor.component_type;
	view.components    = tg3_num_components(accessor.type);
	view.normalized    = accessor.normalized != 0;

	const int32_t componentSize = tg3_component_size(accessor.component_type);
	const size_t  elementSize   = (size_t)componentSize * (size_t)view.components;

	if (accessor.buffer_view < 0 || (uint32_t)accessor.buffer_view >= model.buffer_views_count)
	{
		// A view-less accessor is legal: it means "all zeroes", optionally patched by sparse.
		view.stride = elementSize;
		return view;
	}

	const tg3_buffer_view& bv = model.buffer_views[accessor.buffer_view];
	if (bv.buffer < 0 || (uint32_t)bv.buffer >= model.buffers_count) return view;
	const tg3_buffer& buffer = model.buffers[bv.buffer];
	if (buffer.data.data == nullptr) return view;

	const uint64_t offset = bv.byte_offset + accessor.byte_offset;
	if (offset >= buffer.data.count) return view;

	view.base   = buffer.data.data + offset;
	view.stride = (bv.byte_stride > 0) ? bv.byte_stride : elementSize;
	return view;
}

// One component, widened to float. `normalized` integers map to [0,1] or [-1,1] as the spec
//	says; everything else keeps its numeric value.
float ReadComponent (const uint8_t* p, int32_t componentType, bool normalized)
{
	switch (componentType)
	{
	case TG3_COMPONENT_TYPE_FLOAT: {
		float v; std::memcpy(&v, p, sizeof(v)); return v;
	}
	case TG3_COMPONENT_TYPE_UNSIGNED_BYTE: {
		const uint8_t v = *p;
		return normalized ? (float)v / 255.0f : (float)v;
	}
	case TG3_COMPONENT_TYPE_BYTE: {
		int8_t v; std::memcpy(&v, p, sizeof(v));
		return normalized ? std::max((float)v / 127.0f, -1.0f) : (float)v;
	}
	case TG3_COMPONENT_TYPE_UNSIGNED_SHORT: {
		uint16_t v; std::memcpy(&v, p, sizeof(v));
		return normalized ? (float)v / 65535.0f : (float)v;
	}
	case TG3_COMPONENT_TYPE_SHORT: {
		int16_t v; std::memcpy(&v, p, sizeof(v));
		return normalized ? std::max((float)v / 32767.0f, -1.0f) : (float)v;
	}
	case TG3_COMPONENT_TYPE_UNSIGNED_INT: {
		uint32_t v; std::memcpy(&v, p, sizeof(v)); return (float)v;
	}
	case TG3_COMPONENT_TYPE_INT: {
		int32_t v; std::memcpy(&v, p, sizeof(v)); return (float)v;
	}
	case TG3_COMPONENT_TYPE_DOUBLE: {
		double v; std::memcpy(&v, p, sizeof(v)); return (float)v;
	}
	default: return 0.0f;
	}
}

uint32_t ReadIndexComponent (const uint8_t* p, int32_t componentType)
{
	switch (componentType)
	{
	case TG3_COMPONENT_TYPE_UNSIGNED_BYTE:  return *p;
	case TG3_COMPONENT_TYPE_UNSIGNED_SHORT: { uint16_t v; std::memcpy(&v, p, sizeof(v)); return v; }
	case TG3_COMPONENT_TYPE_UNSIGNED_INT:   { uint32_t v; std::memcpy(&v, p, sizeof(v)); return v; }
	case TG3_COMPONENT_TYPE_SHORT:          { int16_t v; std::memcpy(&v, p, sizeof(v)); return (uint32_t)v; }
	case TG3_COMPONENT_TYPE_BYTE:           { int8_t v; std::memcpy(&v, p, sizeof(v)); return (uint32_t)v; }
	default: return 0;
	}
}

// Flatten an accessor to floats: `out` gets count * components values, tightly packed.
//	Sparse overrides are applied afterwards, which is exactly the order the spec defines.
bool ReadAccessor (const tg3_model& model, int32_t accessorIndex,
	std::vector<float>& out, int32_t& componentsOut)
{
	const AccessorView view = ViewOf(model, accessorIndex);
	if (view.components <= 0) return false;
	componentsOut = view.components;

	const int32_t componentSize = tg3_component_size(view.componentType);
	out.assign(view.count * (size_t)view.components, 0.0f);

	if (view.base != nullptr)
	{
		for (size_t i = 0; i < view.count; ++i)
		{
			const uint8_t* element = view.base + i * view.stride;
			for (int32_t c = 0; c < view.components; ++c)
			{
				out[i * (size_t)view.components + (size_t)c] =
					ReadComponent(element + (size_t)c * (size_t)componentSize,
						view.componentType, view.normalized);
			}
		}
	}

	// Sparse: a list of element indices plus replacement values for exactly those elements.
	//	Quantised or mostly-static data uses it, and skipping it silently corrupts the mesh.
	if (accessorIndex >= 0 && (uint32_t)accessorIndex < model.accessors_count)
	{
		const tg3_accessor& accessor = model.accessors[accessorIndex];
		if (accessor.sparse.is_sparse && accessor.sparse.count > 0)
		{
			const tg3_accessor_sparse_indices& si = accessor.sparse.indices;
			const tg3_accessor_sparse_values&  sv = accessor.sparse.values;
			if (si.buffer_view >= 0 && (uint32_t)si.buffer_view < model.buffer_views_count &&
				sv.buffer_view >= 0 && (uint32_t)sv.buffer_view < model.buffer_views_count)
			{
				const tg3_buffer_view& ibv = model.buffer_views[si.buffer_view];
				const tg3_buffer_view& vbv = model.buffer_views[sv.buffer_view];
				if (ibv.buffer >= 0 && (uint32_t)ibv.buffer < model.buffers_count &&
					vbv.buffer >= 0 && (uint32_t)vbv.buffer < model.buffers_count)
				{
					const uint8_t* ip = model.buffers[ibv.buffer].data.data +
						ibv.byte_offset + si.byte_offset;
					const uint8_t* vp = model.buffers[vbv.buffer].data.data +
						vbv.byte_offset + sv.byte_offset;
					const int32_t indexSize = tg3_component_size(si.component_type);
					for (int32_t s = 0; s < accessor.sparse.count; ++s)
					{
						const uint32_t target = ReadIndexComponent(ip + (size_t)s * indexSize,
							si.component_type);
						if ((size_t)target >= view.count) continue;
						for (int32_t c = 0; c < view.components; ++c)
						{
							const uint8_t* src = vp +
								((size_t)s * (size_t)view.components + (size_t)c) * componentSize;
							out[(size_t)target * (size_t)view.components + (size_t)c] =
								ReadComponent(src, view.componentType, view.normalized);
						}
					}
				}
			}
		}
	}
	return true;
}

bool ReadIndices (const tg3_model& model, int32_t accessorIndex, std::vector<uint32_t>& out)
{
	const AccessorView view = ViewOf(model, accessorIndex);
	if (!view.valid()) return false;
	const int32_t componentSize = tg3_component_size(view.componentType);
	out.resize(view.count);
	for (size_t i = 0; i < view.count; ++i)
		out[i] = ReadIndexComponent(view.base + i * view.stride, view.componentType);
	return true;
}

int32_t FindAttribute (const tg3_primitive& prim, const char* name)
{
	for (uint32_t i = 0; i < prim.attributes_count; ++i)
	{
		const tg3_str& key = prim.attributes[i].key;
		if (key.len == std::strlen(name) && std::strncmp(key.data, name, key.len) == 0)
			return prim.attributes[i].value;
	}
	return -1;
}

// materials

std::string TextureName (ImportContext& ctx, const tg3_model& model, int32_t textureIndex)
{
	if (textureIndex < 0 || (uint32_t)textureIndex >= model.textures_count) return std::string();
	const tg3_texture& texture = model.textures[textureIndex];
	if (texture.source < 0 || (uint32_t)texture.source >= model.images_count) return std::string();
	const tg3_image& image = model.images[texture.source];

	// A GLB keeps its images in a buffer view; a .gltf usually points at a file beside it.
	if (image.buffer_view >= 0 && (uint32_t)image.buffer_view < model.buffer_views_count)
	{
		const tg3_buffer_view& bv = model.buffer_views[image.buffer_view];
		if (bv.buffer >= 0 && (uint32_t)bv.buffer < model.buffers_count)
		{
			const tg3_buffer& buffer = model.buffers[bv.buffer];
			if (buffer.data.data != nullptr)
			{
				return RegisterEmbeddedTexture(ctx, buffer.data.data + bv.byte_offset,
					(size_t)bv.byte_length,
					Str(image.name).empty() ? Str(texture.name) : Str(image.name));
			}
		}
	}
	// images_as_is leaves already-decoded or data-URI bytes here.
	if (image.image.data != nullptr && image.image.count > 0)
	{
		return RegisterEmbeddedTexture(ctx, image.image.data, (size_t)image.image.count,
			Str(image.name));
	}

	const std::string uri = Str(image.uri);
	if (!uri.empty())
	{
		const std::string resolved = ResolveTexturePath(ctx, uri);
		if (!resolved.empty()) ctx.result.textures++;
		return resolved;
	}
	return std::string();
}

Entity ConvertMaterial (ImportContext& ctx, const tg3_model& model, uint32_t index)
{
	Scene& scene = *ctx.scene;
	const tg3_material& src = model.materials[index];
	const std::string name = Str(src.name).empty() ? (ctx.stem + "_material") : Str(src.name);

	const Entity e = CreateMaterial(ctx, name);
	MaterialComponent* m = scene.materials.GetComponent(e);
	if (m == nullptr) return e;

	const tg3_pbr_metallic_roughness& pbr = src.pbr_metallic_roughness;
	m->baseColor = XMFLOAT4((float)pbr.base_color_factor[0], (float)pbr.base_color_factor[1],
		(float)pbr.base_color_factor[2], (float)pbr.base_color_factor[3]);
	m->metalness = (float)pbr.metallic_factor;
	m->roughness = (float)pbr.roughness_factor;
	m->emissiveColor = XMFLOAT4((float)src.emissive_factor[0], (float)src.emissive_factor[1],
		(float)src.emissive_factor[2], 1.0f);

	const std::string alphaMode = Str(src.alpha_mode);
	if (alphaMode == "BLEND")
		m->userBlendMode = wi::enums::BLENDMODE_ALPHA;
	else if (alphaMode == "MASK")
		m->alphaRef = (float)src.alpha_cutoff;

	if (src.double_sided) m->SetDoubleSided(true);

	auto slot = [&](MaterialComponent::TEXTURESLOT s, int32_t textureIndex, int32_t texCoord) {
		const std::string texture = TextureName(ctx, model, textureIndex);
		if (texture.empty()) return;
		m->textures[s].name  = texture;
		m->textures[s].uvset = (texCoord > 0) ? 1u : 0u;
	};
	slot(MaterialComponent::BASECOLORMAP, pbr.base_color_texture.index,
		pbr.base_color_texture.tex_coord);
	// glTF packs occlusion/roughness/metalness into one texture, which is exactly what the
	//	engine's SURFACEMAP is.
	slot(MaterialComponent::SURFACEMAP, pbr.metallic_roughness_texture.index,
		pbr.metallic_roughness_texture.tex_coord);
	slot(MaterialComponent::NORMALMAP, src.normal_texture.index, src.normal_texture.tex_coord);
	slot(MaterialComponent::OCCLUSIONMAP, src.occlusion_texture.index,
		src.occlusion_texture.tex_coord);
	slot(MaterialComponent::EMISSIVEMAP, src.emissive_texture.index,
		src.emissive_texture.tex_coord);

	if (src.normal_texture.index >= 0)
		m->normalMapStrength = (float)src.normal_texture.scale;

	m->SetDirty();
	m->CreateRenderData();
	return e;
}

// meshes

// A glTF mesh is a list of primitives, each with its own material and its own vertex arrays.
//	The engine's MeshComponent is one vertex buffer with subsets, so the primitives are
//	concatenated and each becomes a subset.
Entity ConvertMesh (ImportContext& ctx, const tg3_model& model, uint32_t meshIndex,
	const std::vector<Entity>& materials, Entity armature,
	const std::vector<uint32_t>& jointRemap)
{
	Scene& scene = *ctx.scene;
	const tg3_mesh& src = model.meshes[meshIndex];
	const Entity meshEntity = scene.Entity_CreateMesh(
		UniqueName(scene, Str(src.name).empty() ? (ctx.stem + "_mesh") : Str(src.name)));
	MeshComponent* mesh = scene.meshes.GetComponent(meshEntity);
	if (mesh == nullptr) return meshEntity;

	std::vector<float>    scratch;
	std::vector<uint32_t> indices;
	int32_t components = 0;

	for (uint32_t p = 0; p < src.primitives_count; ++p)
	{
		const tg3_primitive& prim = src.primitives[p];
		// Only triangles are drawn. Point and line primitives are legal glTF but there is
		//	nothing in the engine's mesh path that renders them, so importing them would
		//	produce invisible geometry that still costs memory.
		const int32_t mode = (prim.mode < 0) ? TG3_MODE_TRIANGLES : prim.mode;
		if (mode != TG3_MODE_TRIANGLES) continue;

		const int32_t positionAccessor = FindAttribute(prim, "POSITION");
		if (positionAccessor < 0) continue;
		if (!ReadAccessor(model, positionAccessor, scratch, components) || components < 3)
			continue;

		const uint32_t vertexBase  = (uint32_t)mesh->vertex_positions.size();
		const size_t   vertexCount = scratch.size() / (size_t)components;

		for (size_t i = 0; i < vertexCount; ++i)
		{
			// Mirror Z: right-handed source, left-handed engine.
			mesh->vertex_positions.push_back(XMFLOAT3(
				scratch[i * components + 0],
				scratch[i * components + 1],
				-scratch[i * components + 2]));
		}

		auto readInto = [&](const char* attribute, auto&& fn) -> bool {
			const int32_t accessor = FindAttribute(prim, attribute);
			if (accessor < 0) return false;
			int32_t n = 0;
			if (!ReadAccessor(model, accessor, scratch, n)) return false;
			const size_t count = scratch.size() / (size_t)std::max(n, 1);
			for (size_t i = 0; i < count && i < vertexCount; ++i)
				fn(i, scratch.data() + i * (size_t)n, n);
			return true;
		};

		const bool hadNormals = readInto("NORMAL", [&](size_t, const float* v, int32_t n) {
			mesh->vertex_normals.push_back(XMFLOAT3(v[0], n > 1 ? v[1] : 0.0f,
				n > 2 ? -v[2] : 0.0f));
		});
		if (!hadNormals && !mesh->vertex_normals.empty())
		{
			// Some primitives had normals and this one does not; pad so the arrays stay the
			//	same length as the position array or every later index is off by the shortfall.
			mesh->vertex_normals.resize(mesh->vertex_positions.size(), XMFLOAT3(0, 1, 0));
		}

		readInto("TANGENT", [&](size_t, const float* v, int32_t n) {
			// w carries the bitangent sign and must NOT be mirrored, only x/y/z are.
			mesh->vertex_tangents.push_back(XMFLOAT4(v[0], n > 1 ? v[1] : 0.0f,
				n > 2 ? -v[2] : 0.0f, n > 3 ? v[3] : 1.0f));
		});
		readInto("TEXCOORD_0", [&](size_t, const float* v, int32_t n) {
			mesh->vertex_uvset_0.push_back(XMFLOAT2(v[0], n > 1 ? v[1] : 0.0f));
		});
		readInto("TEXCOORD_1", [&](size_t, const float* v, int32_t n) {
			mesh->vertex_uvset_1.push_back(XMFLOAT2(v[0], n > 1 ? v[1] : 0.0f));
		});
		readInto("COLOR_0", [&](size_t, const float* v, int32_t n) {
			mesh->vertex_colors.push_back(wi::Color::fromFloat4(XMFLOAT4(
				v[0], n > 1 ? v[1] : 1.0f, n > 2 ? v[2] : 1.0f, n > 3 ? v[3] : 1.0f)).rgba);
		});

		if (armature != INVALID_ENTITY)
		{
			const size_t boneBase = mesh->vertex_boneindices.size();
			readInto("JOINTS_0", [&](size_t, const float* v, int32_t n) {
				// JOINTS_0 indexes the SKIN's joint list; the armature stores bones in that
				//	same order, so jointRemap is identity unless a joint was dropped.
				auto remap = [&](float f) -> uint32_t {
					const uint32_t j = (uint32_t)f;
					return (j < jointRemap.size()) ? jointRemap[j] : 0u;
				};
				mesh->vertex_boneindices.push_back(XMUINT4(remap(v[0]),
					n > 1 ? remap(v[1]) : 0u, n > 2 ? remap(v[2]) : 0u,
					n > 3 ? remap(v[3]) : 0u));
			});
			readInto("WEIGHTS_0", [&](size_t, const float* v, int32_t n) {
				mesh->vertex_boneweights.push_back(XMFLOAT4(v[0], n > 1 ? v[1] : 0.0f,
					n > 2 ? v[2] : 0.0f, n > 3 ? v[3] : 0.0f));
			});
			// A primitive without skin attributes inside a skinned mesh still needs entries,
			//	or the bone arrays fall out of step with the positions.
			mesh->vertex_boneindices.resize(mesh->vertex_positions.size(), XMUINT4(0, 0, 0, 0));
			mesh->vertex_boneweights.resize(mesh->vertex_positions.size(), XMFLOAT4(0, 0, 0, 0));
			(void)boneBase;
		}

		MeshComponent::MeshSubset subset;
		subset.indexOffset = (uint32_t)mesh->indices.size();
		subset.materialID = (prim.material >= 0 && (size_t)prim.material < materials.size())
			? materials[prim.material] : INVALID_ENTITY;

		if (prim.indices >= 0 && ReadIndices(model, prim.indices, indices))
		{
			for (uint32_t index : indices)
				mesh->indices.push_back(vertexBase + index);
		}
		else
		{
			// Non-indexed primitive: the vertices are the triangle list.
			for (size_t i = 0; i < vertexCount; ++i)
				mesh->indices.push_back(vertexBase + (uint32_t)i);
		}

		subset.indexCount = (uint32_t)mesh->indices.size() - subset.indexOffset;
		if (subset.indexCount > 0)
			mesh->subsets.push_back(subset);
	}

	if (armature != INVALID_ENTITY)
		mesh->armatureID = armature;

	FinalizeMesh(ctx, meshEntity, /*flipWinding=*/true);
	return meshEntity;
}

// animation

void AddChannel (Scene& scene, AnimationComponent& anim, Entity target,
	AnimationComponent::AnimationChannel::Path path,
	const std::vector<float>& times, const std::vector<float>& values,
	AnimationComponent::AnimationSampler::Mode mode)
{
	if (times.empty() || values.empty()) return;

	const Entity dataEntity = wi::ecs::CreateEntity();
	AnimationDataComponent& data = scene.animation_datas.Create(dataEntity);
	data.keyframe_times.resize(times.size());
	std::copy(times.begin(), times.end(), data.keyframe_times.begin());
	data.keyframe_data.resize(values.size());
	std::copy(values.begin(), values.end(), data.keyframe_data.begin());

	AnimationComponent::AnimationSampler sampler;
	sampler.data = dataEntity;
	sampler.mode = mode;
	anim.samplers.push_back(sampler);

	AnimationComponent::AnimationChannel channel;
	channel.target = target;
	channel.path = path;
	channel.samplerIndex = (int)anim.samplers.size() - 1;
	anim.channels.push_back(channel);
}

} // namespace

// the import

bool ImportGLTF (ImportContext& ctx, Entity root)
{
	Scene& scene = *ctx.scene;

	std::vector<uint8_t> bytes;
	if (!ReadFile(ctx.sourcePath, bytes))
	{
		ctx.result.error = "cannot read " + ctx.sourcePath;
		return false;
	}

	tg3_parse_options options;
	tg3_parse_options_init(&options);
	options.images_as_is        = 1;   // the resource manager decodes, not tinygltf
	options.validate_indices    = 1;   // refuse out-of-range indices rather than dereference them
	options.fs.file_exists      = &FsFileExists;
	options.fs.read_file        = &FsReadFile;
	options.fs.free_file        = &FsFreeFile;
	options.fs.get_file_size    = &FsGetFileSize;
	options.fs.user_data        = &ctx;

	tg3_model model;
	std::memset(&model, 0, sizeof(model));
	tg3_error_stack errors;
	tg3_error_stack_init(&errors);

	const tg3_error_code code = tg3_parse_auto(&model, &errors, bytes.data(), bytes.size(),
		ctx.baseDir.c_str(), (uint32_t)ctx.baseDir.size(), &options);

	if (code != TG3_OK || tg3_errors_has_error(&errors))
	{
		std::string message = "tinygltf: parse failed";
		for (uint32_t i = 0; i < errors.count; ++i)
		{
			if (errors.entries[i].message != nullptr)
			{
				message = std::string("tinygltf: ") + errors.entries[i].message;
				break;   // the first error is the one that caused the rest
			}
		}
		ctx.result.error = message;
		tg3_model_free(&model);
		return false;
	}

	// materials
	std::vector<Entity> materials(model.materials_count, INVALID_ENTITY);
	for (uint32_t i = 0; i < model.materials_count; ++i)
		materials[i] = ConvertMaterial(ctx, model, i);

	// nodes
	std::vector<Entity> nodeEntities(model.nodes_count, INVALID_ENTITY);
	for (uint32_t i = 0; i < model.nodes_count; ++i)
	{
		const tg3_node& node = model.nodes[i];
		const Entity e = scene.Entity_CreateTransform(
			UniqueName(scene, Str(node.name).empty() ? "node" : Str(node.name)));
		nodeEntities[i] = e;
		ctx.result.nodes++;

		TransformComponent* t = scene.transforms.GetComponent(e);
		if (t == nullptr) continue;

		if (node.has_matrix)
		{
			// A column-major glTF matrix, decomposed and then mirrored through Z. Handling
			//	the matrix case by decomposition keeps one mirroring rule instead of two.
			const XMMATRIX m = XMMatrixSet(
				(float)node.matrix[0],  (float)node.matrix[1],  (float)node.matrix[2],  (float)node.matrix[3],
				(float)node.matrix[4],  (float)node.matrix[5],  (float)node.matrix[6],  (float)node.matrix[7],
				(float)node.matrix[8],  (float)node.matrix[9],  (float)node.matrix[10], (float)node.matrix[11],
				(float)node.matrix[12], (float)node.matrix[13], (float)node.matrix[14], (float)node.matrix[15]);
			XMVECTOR s, r, p;
			if (XMMatrixDecompose(&s, &r, &p, m))
			{
				XMFLOAT3 scale; XMStoreFloat3(&scale, s);
				XMFLOAT4 rot;   XMStoreFloat4(&rot, r);
				XMFLOAT3 pos;   XMStoreFloat3(&pos, p);
				t->scale_local       = scale;
				t->rotation_local    = XMFLOAT4(-rot.x, -rot.y, rot.z, rot.w);
				t->translation_local = XMFLOAT3(pos.x, pos.y, -pos.z);
			}
		}
		else
		{
			t->translation_local = XMFLOAT3((float)node.translation[0],
				(float)node.translation[1], -(float)node.translation[2]);
			// Mirroring the Z axis turns quaternion (x,y,z,w) into (-x,-y,z,w).
			t->rotation_local = XMFLOAT4(-(float)node.rotation[0], -(float)node.rotation[1],
				(float)node.rotation[2], (float)node.rotation[3]);
			t->scale_local = XMFLOAT3((float)node.scale[0], (float)node.scale[1],
				(float)node.scale[2]);
		}
		t->SetDirty();
	}

	// Parent after creating them all, so a forward reference is never a problem.
	std::vector<bool> hasParent(model.nodes_count, false);
	for (uint32_t i = 0; i < model.nodes_count; ++i)
	{
		const tg3_node& node = model.nodes[i];
		for (uint32_t c = 0; c < node.children_count; ++c)
		{
			const int32_t child = node.children[c];
			if (child < 0 || (uint32_t)child >= model.nodes_count) continue;
			scene.Component_Attach(nodeEntities[child], nodeEntities[i], true);
			hasParent[child] = true;
		}
	}
	for (uint32_t i = 0; i < model.nodes_count; ++i)
		if (!hasParent[i])
			scene.Component_Attach(nodeEntities[i], root, true);

	// skins
	std::vector<Entity>                armatureForSkin(model.skins_count, INVALID_ENTITY);
	std::vector<std::vector<uint32_t>> jointRemapForSkin(model.skins_count);
	if (ctx.options.importSkins)
	{
		for (uint32_t s = 0; s < model.skins_count; ++s)
		{
			const tg3_skin& skin = model.skins[s];
			const Entity armatureEntity = scene.Entity_CreateTransform(
				UniqueName(scene, Str(skin.name).empty() ? (ctx.stem + "_armature") : Str(skin.name)));
			scene.Component_Attach(armatureEntity, root, true);
			ArmatureComponent& armature = scene.armatures.Create(armatureEntity);

			std::vector<float> bindMatrices;
			int32_t components = 0;
			const bool haveBind = (skin.inverse_bind_matrices >= 0) &&
				ReadAccessor(model, skin.inverse_bind_matrices, bindMatrices, components) &&
				components == 16;

			jointRemapForSkin[s].assign(skin.joints_count, 0);
			for (uint32_t j = 0; j < skin.joints_count; ++j)
			{
				const int32_t joint = skin.joints[j];
				if (joint < 0 || (uint32_t)joint >= model.nodes_count) continue;
				jointRemapForSkin[s][j] = (uint32_t)armature.boneCollection.size();
				armature.boneCollection.push_back(nodeEntities[joint]);

				XMFLOAT4X4 inverseBind = wi::math::IDENTITY_MATRIX;
				if (haveBind && (size_t)(j + 1) * 16 <= bindMatrices.size())
				{
					const float* v = bindMatrices.data() + (size_t)j * 16;
					// glTF stores column-major; XMFLOAT4X4 is row-major, so this transposes
					//	while it copies.
					const XMMATRIX m = XMMatrixSet(
						v[0],  v[1],  v[2],  v[3],
						v[4],  v[5],  v[6],  v[7],
						v[8],  v[9],  v[10], v[11],
						v[12], v[13], v[14], v[15]);
					// The bind pose lives in the mirrored space too: M' = S * M * S with
					//	S = diag(1,1,-1,1), which is what conjugating by the Z flip means.
					const XMMATRIX flip = XMMatrixScaling(1, 1, -1);
					XMStoreFloat4x4(&inverseBind, flip * m * flip);
				}
				armature.inverseBindMatrices.push_back(inverseBind);
			}
			ctx.result.bones += (uint32_t)armature.boneCollection.size();
			armatureForSkin[s] = armatureEntity;
		}
	}

	// meshes
	// A glTF mesh instanced by several nodes is converted once per (mesh, skin) pair: the
	//	skin changes the vertex data, so two nodes with different skins genuinely need two.
	std::unordered_map<uint64_t, Entity> meshCache;
	for (uint32_t i = 0; i < model.nodes_count; ++i)
	{
		const tg3_node& node = model.nodes[i];
		if (node.mesh < 0 || (uint32_t)node.mesh >= model.meshes_count) continue;

		const int32_t skinIndex = ctx.options.importSkins ? node.skin : -1;
		const uint64_t key = ((uint64_t)(uint32_t)node.mesh << 32) |
			(uint32_t)(skinIndex + 1);

		auto it = meshCache.find(key);
		if (it == meshCache.end())
		{
			const Entity armature = (skinIndex >= 0 && (uint32_t)skinIndex < armatureForSkin.size())
				? armatureForSkin[skinIndex] : INVALID_ENTITY;
			static const std::vector<uint32_t> kNoRemap;
			const std::vector<uint32_t>& remap =
				(skinIndex >= 0 && (uint32_t)skinIndex < jointRemapForSkin.size())
					? jointRemapForSkin[skinIndex] : kNoRemap;
			it = meshCache.emplace(key,
				ConvertMesh(ctx, model, (uint32_t)node.mesh, materials, armature, remap)).first;
		}

		ObjectComponent& object = scene.objects.Create(nodeEntities[i]);
		object.meshID = it->second;
	}

	// animations
	if (ctx.options.importAnimations)
	{
		std::vector<float> times, values;
		int32_t components = 0;
		for (uint32_t a = 0; a < model.animations_count; ++a)
		{
			const tg3_animation& src = model.animations[a];
			const Entity animEntity = scene.Entity_CreateTransform(
				UniqueName(scene, Str(src.name).empty() ? (ctx.stem + "_anim") : Str(src.name)));
			scene.Component_Attach(animEntity, root, true);
			AnimationComponent& anim = scene.animations.Create(animEntity);
			anim.SetLooped(true);

			float start = FLT_MAX;
			float end   = -FLT_MAX;

			for (uint32_t c = 0; c < src.channels_count; ++c)
			{
				const tg3_animation_channel& channel = src.channels[c];
				if (channel.sampler < 0 || (uint32_t)channel.sampler >= src.samplers_count) continue;
				const int32_t targetNode = channel.target.node;
				if (targetNode < 0 || (uint32_t)targetNode >= model.nodes_count) continue;

				const tg3_animation_sampler& sampler = src.samplers[channel.sampler];
				if (!ReadAccessor(model, sampler.input, times, components)) continue;
				if (!ReadAccessor(model, sampler.output, values, components)) continue;
				if (times.empty()) continue;

				start = std::min(start, times.front());
				end   = std::max(end, times.back());

				const std::string interpolation = Str(sampler.interpolation);
				AnimationComponent::AnimationSampler::Mode mode =
					AnimationComponent::AnimationSampler::LINEAR;
				if (interpolation == "STEP")
					mode = AnimationComponent::AnimationSampler::STEP;
				else if (interpolation == "CUBICSPLINE")
					mode = AnimationComponent::AnimationSampler::CUBICSPLINE;

				const std::string path = Str(channel.target.path);
				if (path == "translation")
				{
					// Mirror every Z, including the in- and out-tangents a cubic spline
					//	stores around each value.
					for (size_t v = 2; v < values.size(); v += 3) values[v] = -values[v];
					AddChannel(scene, anim, nodeEntities[targetNode],
						AnimationComponent::AnimationChannel::Path::TRANSLATION, times, values, mode);
				}
				else if (path == "rotation")
				{
					for (size_t v = 0; v + 3 < values.size(); v += 4)
					{
						values[v + 0] = -values[v + 0];
						values[v + 1] = -values[v + 1];
					}
					AddChannel(scene, anim, nodeEntities[targetNode],
						AnimationComponent::AnimationChannel::Path::ROTATION, times, values, mode);
				}
				else if (path == "scale")
				{
					AddChannel(scene, anim, nodeEntities[targetNode],
						AnimationComponent::AnimationChannel::Path::SCALE, times, values, mode);
				}
				else if (path == "weights")
				{
					AddChannel(scene, anim, nodeEntities[targetNode],
						AnimationComponent::AnimationChannel::Path::WEIGHTS, times, values, mode);
				}
			}

			if (anim.channels.empty())
			{
				scene.Entity_Remove(animEntity, true);
			}
			else
			{
				anim.start = (start == FLT_MAX) ? 0.0f : start;
				anim.end   = (end == -FLT_MAX) ? 0.0f : end;
				ctx.result.animations++;
			}
		}
	}

	tg3_model_free(&model);
	return true;
}

} // namespace st::model::detail
