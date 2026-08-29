#pragma once
// Shared plumbing for the model importers. Internal to Framework/io/model — the public
//	surface is ModelImporter.h.
//
//	Both backends face the same three problems, and they are solved once here:
//
//	  reading      every byte a loader sees comes through wi::helper::FileRead, so a model
//	               inside a mounted asset package works
//	  textures     a reference is either a path (resolve against the model's own folder) or
//	               embedded bytes (register with wi::resourcemanager under a synthetic name)
//	  finishing    a built MeshComponent needs normals it may not have, an AABB, and
//	               CreateRenderData before it can be drawn

#include "ModelImporter.h"

#include "wiECS.h"
#include "wiScene.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace st::model::detail {

// Everything an importer accumulates while it runs. One per Import() call.
struct ImportContext {
	wi::scene::Scene* scene = nullptr;
	std::string       sourcePath;   // as given to Import()
	std::string       baseDir;      // sourcePath's folder, with a trailing '/' (may be empty)
	std::string       stem;         // file name without directory or extension
	ImportOptions     options;
	ImportResult      result;

	// Synthetic names already registered for embedded images, keyed by content hash, so a
	//	texture reused by twenty materials is registered once.
	std::unordered_map<uint64_t, std::string> embeddedTextures;
};

// Read a whole file through the engine's file layer (packs first, then disk).
bool ReadFile (const std::string& path, std::vector<uint8_t>& out);

// path -> "dir/" (empty when there is no directory part). Always forward slashes.
std::string DirectoryOf (const std::string& path);
// path -> file name without directory or extension.
std::string StemOf (const std::string& path);
// Lowercased extension without the dot.
std::string ExtensionOf (const std::string& path);

// Resolve a texture reference a model file gave us into a name the engine can load.
//
//	`reference` is whatever the format stored — usually relative, sometimes an absolute path
//	from the exporting machine, sometimes a data: URI the loader already decoded. It is tried
//	as-is first (so a name the asset package holds wins), then relative to the model's own
//	folder, then by file name alone in that folder — which is what rescues the very common
//	case of an FBX pointing at "C:/Users/someone/textures/wall.png".
//
//	Returns an empty string when nothing resolves, and the caller should leave the slot empty
//	rather than store a name that will never load.
std::string ResolveTexturePath (ImportContext& ctx, const std::string& reference);

// Register embedded image bytes with the resource manager and return the synthetic name to
//	put in a material slot. Deduplicated by content hash. `hint` only shapes the name, so a
//	human reading the Resource Explorer can tell what it is.
std::string RegisterEmbeddedTexture (ImportContext& ctx, const uint8_t* data, size_t size,
	const std::string& hint);

// Make a mesh drawable: fill in missing normals, compute the AABB, upload.
//	`flipWinding` reverses every triangle, which is how a right-handed source becomes
//	left-handed geometry.
void FinalizeMesh (ImportContext& ctx, wi::ecs::Entity meshEntity, bool flipWinding);

// Create a material entity with sane defaults and a unique name.
wi::ecs::Entity CreateMaterial (ImportContext& ctx, const std::string& name);

// A name no other entity in the scene is using, derived from `base`.
std::string UniqueName (wi::scene::Scene& scene, const std::string& base);

// The two backends. Each builds under `root`, which the dispatcher has already created.
//	Both return false with ctx.result.error set on failure.
bool ImportGLTF (ImportContext& ctx, wi::ecs::Entity root);
bool ImportUFBX (ImportContext& ctx, wi::ecs::Entity root);

} // namespace st::model::detail
