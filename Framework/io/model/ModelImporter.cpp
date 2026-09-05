#include "ModelImporterCommon.h"

#include "io/asset/StHash.h"

#include "wiHelper.h"
#include "wiBacklog.h"
#include "wiResourceManager.h"
#include "wiVector.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

using wi::ecs::Entity;
using wi::ecs::INVALID_ENTITY;
using namespace wi::scene;

namespace st::model {

namespace {

// One row per extension the build can read. The editor's dialog filter and its drop test are
//	both generated from this, so a new backend is one edit here rather than three.
const char* const kExtensions[] = { "gltf", "glb", "fbx", "obj" };

} // namespace

const char* const* SupportedExtensions (size_t& count)
{
	count = sizeof(kExtensions) / sizeof(kExtensions[0]);
	return kExtensions;
}

bool CanImport (const std::string& path)
{
	const std::string ext = detail::ExtensionOf(path);
	if (ext.empty()) return false;
	for (const char* candidate : kExtensions)
		if (ext == candidate) return true;
	return false;
}

ImportResult Import (Scene& scene, const std::string& path, const ImportOptions& options)
{
	detail::ImportContext ctx;
	ctx.scene      = &scene;
	ctx.sourcePath = path;
	ctx.baseDir    = detail::DirectoryOf(path);
	ctx.stem       = detail::StemOf(path);
	ctx.options    = options;

	const std::string ext = detail::ExtensionOf(path);
	if (!CanImport(path))
	{
		ctx.result.error = "no importer for ." + ext;
		return ctx.result;
	}

	// The root exists before the backend runs, so a backend can parent to it as it goes and
	//	a failure part-way through still leaves one entity to clean up rather than a hundred
	//	loose ones.
	const Entity root = scene.Entity_CreateTransform(detail::UniqueName(scene, ctx.stem));
	ctx.result.root = root;

	const bool ok = (ext == "gltf" || ext == "glb")
		? detail::ImportGLTF(ctx, root)
		: detail::ImportUFBX(ctx, root);

	if (!ok)
	{
		// Take the root back out: a failed import must not leave a stub in the Hierarchy that
		//	looks like it worked.
		scene.Entity_Remove(root, true);
		ctx.result.root = INVALID_ENTITY;
		if (ctx.result.error.empty())
			ctx.result.error = "import failed: " + path;
		return ctx.result;
	}

	if (options.scale != 1.0f)
	{
		if (TransformComponent* t = scene.transforms.GetComponent(root))
		{
			t->scale_local = XMFLOAT3(options.scale, options.scale, options.scale);
			t->SetDirty();
		}
	}

	scene.Update(0.0f); // resolve the hierarchy so world matrices exist before the caller places it
	return ctx.result;
}

} // namespace st::model

namespace st::model::detail {

// paths

std::string DirectoryOf (const std::string& path)
{
	const size_t slash = path.find_last_of("/\\");
	if (slash == std::string::npos) return std::string();
	std::string dir = path.substr(0, slash + 1);
	for (char& c : dir) if (c == '\\') c = '/';
	return dir;
}

std::string StemOf (const std::string& path)
{
	const size_t slash = path.find_last_of("/\\");
	const std::string file = (slash == std::string::npos) ? path : path.substr(slash + 1);
	const size_t dot = file.find_last_of('.');
	return (dot == std::string::npos) ? file : file.substr(0, dot);
}

std::string ExtensionOf (const std::string& path)
{
	const size_t dot = path.find_last_of('.');
	if (dot == std::string::npos) return std::string();
	// A dot in a directory name is not an extension.
	const size_t slash = path.find_last_of("/\\");
	if (slash != std::string::npos && dot < slash) return std::string();
	std::string ext = path.substr(dot + 1);
	for (char& c : ext) c = (char)std::tolower((unsigned char)c);
	return ext;
}

bool ReadFile (const std::string& path, std::vector<uint8_t>& out)
{
	wi::vector<uint8_t> buffer;
	if (!wi::helper::FileRead(path, buffer))
		return false;
	out.assign(buffer.begin(), buffer.end());
	return true;
}

// textures

std::string ResolveTexturePath (ImportContext& ctx, const std::string& reference)
{
	if (reference.empty()) return std::string();

	std::string ref = reference;
	for (char& c : ref) if (c == '\\') c = '/';

	auto exists = [](const std::string& p) { return !p.empty() && wi::helper::FileExists(p); };

	// As given. A logical path the asset package already holds hits here, and so does a
	//	relative name when the working directory happens to line up.
	if (exists(ref)) return ref;

	// Relative to the model itself, which is where an exporter means it.
	if (!ctx.baseDir.empty())
	{
		const std::string sibling = ctx.baseDir + ref;
		if (exists(sibling)) return sibling;
	}

	// File name alone, next to the model. This is the case that matters in practice: an FBX
	//	written on another machine names "D:/work/proj/tex/wall.png", and the texture that
	//	actually shipped sits beside the .fbx.
	const size_t slash = ref.find_last_of('/');
	if (slash != std::string::npos)
	{
		const std::string leaf = ref.substr(slash + 1);
		if (!ctx.baseDir.empty())
		{
			const std::string sibling = ctx.baseDir + leaf;
			if (exists(sibling)) return sibling;
		}
		if (exists(leaf)) return leaf;
	}

	return std::string();
}

std::string RegisterEmbeddedTexture (ImportContext& ctx, const uint8_t* data, size_t size,
	const std::string& hint)
{
	if (data == nullptr || size == 0) return std::string();

	// Hash the bytes, not the name: a .glb routinely stores one image and points six
	//	materials at it, and the exporter often gives them all the same empty name.
	const uint64_t hash = st::asset::Hash64(data, size, 0);
	auto it = ctx.embeddedTextures.find(hash);
	if (it != ctx.embeddedTextures.end())
		return it->second;

	// The name never touches the filesystem - it is a resource-manager key. Prefixed so it is
	//	obvious in the Resource Explorer where it came from, and hashed so two models with a
	//	"texture0" each do not collide.
	char suffix[32];
	std::snprintf(suffix, sizeof(suffix), "%016llx", (unsigned long long)hash);
	std::string name = "embedded/" + ctx.stem + "/" +
		(hint.empty() ? std::string("texture") : hint) + "_" + suffix;

	wi::resourcemanager::Load(name, wi::resourcemanager::Flags::NONE, data, size);
	ctx.embeddedTextures[hash] = name;
	ctx.result.textures++;
	return name;
}

// mesh

void FinalizeMesh (ImportContext& ctx, Entity meshEntity, bool flipWinding)
{
	MeshComponent* mesh = ctx.scene->meshes.GetComponent(meshEntity);
	if (mesh == nullptr) return;

	if (flipWinding)
	{
		// Mirroring one axis reverses triangle winding, so every face would be backfacing
		//	and the model would render inside-out.
		for (size_t i = 0; i + 2 < mesh->indices.size(); i += 3)
			std::swap(mesh->indices[i + 1], mesh->indices[i + 2]);
	}

	if (mesh->vertex_normals.empty() && ctx.options.generateMissingNormals)
		mesh->ComputeNormals(MeshComponent::COMPUTE_NORMALS_SMOOTH_FAST);

	// Subsets with no material still have to name one, or the renderer has nothing to bind.
	for (MeshComponent::MeshSubset& subset : mesh->subsets)
	{
		if (subset.materialID == INVALID_ENTITY)
			subset.materialID = CreateMaterial(ctx, ctx.stem + "_default");
	}

	mesh->CreateRenderData();
	ctx.result.meshes++;
}

Entity CreateMaterial (ImportContext& ctx, const std::string& name)
{
	Scene& scene = *ctx.scene;
	const Entity e = scene.Entity_CreateMaterial(UniqueName(scene, name));
	ctx.result.materials++;
	return e;
}

std::string UniqueName (Scene& scene, const std::string& base)
{
	const std::string root = base.empty() ? std::string("Imported") : base;
	if (scene.Entity_FindByName(root) == INVALID_ENTITY)
		return root;
	for (int i = 1; i < 100000; ++i)
	{
		const std::string candidate = root + "_" + std::to_string(i);
		if (scene.Entity_FindByName(candidate) == INVALID_ENTITY)
			return candidate;
	}
	return root;
}

} // namespace st::model::detail
