#pragma once
// Import a standard model file straight into a wi::scene::Scene.
//
//	The engine's own wi::scene::LoadModel reads ONE format: .wiscene, which is a wi::Archive
//	of an already-built scene. That is the right shipping format and the wrong authoring one
//	nothing exports it. This is the other half: the four interchange formats an art pipeline
//	actually produces, converted into engine components on load.
//
//	  .gltf / .glb   glTF 2.0, via tinygltf v3
//	  .fbx           Autodesk FBX (binary and ASCII), via ufbx
//	  .obj           Wavefront OBJ + its .mtl, also via ufbx
//
//	Why these two libraries
//
//	Both are C. This workspace compiles with /EHsc- /GR- and _HAS_EXCEPTIONS=0, so a C++
//	loader that throws is not a drop-in: it would have to be built with its own exception
//	settings behind a C boundary, or run in a mode where a malformed file calls std::abort().
//	A C parser reports failure by return code, which is what the rest of Framework/io does.
//	ufbx reading OBJ as well as FBX is why two libraries cover four extensions.
//
//	Everything goes through the engine's file layer
//
//	Neither library is allowed to open a file itself. The importers read bytes with
//	wi::helper::FileRead and hand the loaders memory, and both are given callbacks for the
//	side files a model pulls in (a .bin next to a .gltf, an .mtl next to an .obj, the
//	textures either references). So a model inside a mounted asset package imports exactly
//	like one on disk, and a texture it names resolves the same way afterwards.
//
//	Embedded textures - a .glb with its images inline, an .fbx with its content baked in
//	are registered with wi::resourcemanager under a synthetic name, and the material points
//	at that name. Nothing is written to disk.
//
//	What comes across
//
//	Node hierarchy, meshes (positions, normals, tangents, two UV sets, vertex colours),
//	per-face materials as mesh subsets, PBR materials with their texture slots, skins as
//	ArmatureComponents with inverse bind matrices, and animations as AnimationComponents
//	over AnimationDataComponent curves. Cameras and punctual lights are optional.
//
//	Coordinate systems are converted to the engine's left-handed, Y-up space: ufbx does it
//	during load, and the glTF importer mirrors Z and flips triangle winding on the way in.

#include "wiECS.h"
#include "wiScene_Decl.h"

#include <cstdint>
#include <string>

namespace wi::scene { struct Scene; }

namespace st::model {

struct ImportOptions {
	// Off-by-default extras: a prop rarely wants the exporter's camera rig or its lights,
	//	and importing them means hunting them down in the Hierarchy afterwards.
	bool  importCameras    = false;
	bool  importLights     = false;
	bool  importAnimations = true;
	bool  importSkins      = true;
	// Uniform scale applied to the root. FBX is authored in centimetres often enough that
	//	the FBX path normalises to metres on its own; this is on top of that.
	float scale            = 1.0f;
	// Generate normals for meshes that ship without them, rather than importing a mesh that
	//	renders black.
	bool  generateMissingNormals = true;
};

struct ImportResult {
	wi::ecs::Entity root = wi::ecs::INVALID_ENTITY;

	uint32_t meshes     = 0;
	uint32_t materials  = 0;
	uint32_t textures   = 0;   // distinct texture names referenced
	uint32_t animations = 0;
	uint32_t bones      = 0;
	uint32_t nodes      = 0;

	std::string error;         // empty on success
	bool ok () const { return root != wi::ecs::INVALID_ENTITY; }
};

// Lowercase extensions this build can import, without the dot. The editor builds its file
//	dialog filter and its drop test from this, so adding a backend does not mean editing the
//	editor as well.
const char* const* SupportedExtensions (size_t& count);

// True when Import() would take this path (extension test only; the file is not opened).
bool CanImport (const std::string& path);

// Read `path` and build it into `scene` under a single new root entity, which the caller
//	owns and can place. Returns a result whose `root` is INVALID_ENTITY on failure, with
//	`error` saying why. Never throws; never partially reports success.
//
//	The root is created with a TransformComponent at the origin: placement is the caller's
//	job, because only the caller knows where "here" is.
ImportResult Import (wi::scene::Scene& scene, const std::string& path,
	const ImportOptions& options = ImportOptions());

} // namespace st::model
