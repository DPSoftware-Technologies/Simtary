// model_import_test — the third-party half of Framework/io/model.
//
//	Links tinygltf and ufbx and nothing else: no engine, no graphics device. That is a
//	deliberate split. The engine-facing half of the importers (components, skinning,
//	handedness) cannot run without a device, but the half that actually breaks when a
//	dependency is updated is this one — the API shapes, the parse options, the accessor
//	layout — and that half needs no device at all.
//
//	Each case writes a small model to a temp directory, parses it back, and checks the
//	numbers that the importers depend on being right.

#include <tiny_gltf_v3.h>
#include <ufbx.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void Check (bool condition, const char* what)
{
    if (condition) {
        std::printf("  ok    %s\n", what);
    } else {
        std::printf("  FAIL  %s\n", what);
        ++g_failures;
    }
}

bool WriteText (const fs::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(text.data(), (std::streamsize)text.size());
    return (bool)out;
}

bool WriteBinary (const fs::path& path, const void* data, size_t size)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write((const char*)data, (std::streamsize)size);
    return (bool)out;
}

std::vector<uint8_t> ReadAll (const fs::path& path)
{
    std::vector<uint8_t> bytes;
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return bytes;
    const std::streamoff size = in.tellg();
    if (size <= 0) return bytes;
    bytes.resize((size_t)size);
    in.seekg(0);
    in.read((char*)bytes.data(), size);
    if (in.gcount() != size) bytes.clear();
    return bytes;
}

// ── OBJ, through ufbx ─────────────────────────────────────────────────────────
// One quad with a material, which is enough to exercise the path the OBJ importer
//	depends on: faces that are NOT triangles, a UV set, and an .mtl found beside the .obj.

void TestOBJ (const fs::path& dir)
{
    std::printf("OBJ (ufbx)\n");

    const fs::path objPath = dir / "quad.obj";
    Check(WriteText(dir / "quad.mtl",
        "newmtl surface\n"
        "Kd 0.8 0.2 0.1\n"
        "Ns 40\n"), "write quad.mtl");
    Check(WriteText(objPath,
        "mtllib quad.mtl\n"
        "v -1 0 -1\nv 1 0 -1\nv 1 0 1\nv -1 0 1\n"
        "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
        "vn 0 1 0\n"
        "usemtl surface\n"
        "f 1/1/1 2/2/1 3/3/1 4/4/1\n"), "write quad.obj");

    const std::vector<uint8_t> bytes = ReadAll(objPath);
    Check(!bytes.empty(), "read quad.obj back");

    // The same options the importer uses, so a change there is caught here.
    ufbx_load_opts opts = {};
    opts.target_axes                = ufbx_axes_left_handed_y_up;
    opts.target_unit_meters         = 1.0f;
    opts.space_conversion           = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    opts.handedness_conversion_axis = UFBX_MIRROR_AXIS_Z;
    opts.generate_missing_normals   = true;
    opts.load_external_files        = true;
    opts.ignore_missing_external_files = true;
    opts.obj_search_mtl_by_filename = true;
    const std::string filename = objPath.string();
    opts.filename = ufbx_string{ filename.c_str(), filename.size() };

    ufbx_error error;
    ufbx_scene* scene = ufbx_load_memory(bytes.data(), bytes.size(), &opts, &error);
    if (scene == nullptr) {
        char description[512];
        ufbx_format_error(description, sizeof(description), &error);
        std::printf("  FAIL  ufbx_load_memory: %s\n", description);
        ++g_failures;
        return;
    }

    Check(scene->meshes.count == 1, "one mesh");
    if (scene->meshes.count > 0) {
        const ufbx_mesh* mesh = scene->meshes.data[0];
        Check(mesh->num_faces == 1, "one face");
        Check(mesh->num_vertices == 4, "four vertices");
        Check(mesh->vertex_normal.exists, "normals present (generated)");
        Check(mesh->vertex_uv.exists, "uv set present");
        // A quad is one face of four corners: the importer triangulates it into two
        //	triangles, and max_face_triangles is the buffer size that has to be right.
        Check(mesh->max_face_triangles == 2, "quad triangulates to 2");

        std::vector<uint32_t> tri(mesh->max_face_triangles * 3 + 3);
        const uint32_t triCount = ufbx_triangulate_face(tri.data(), tri.size(), mesh,
            mesh->faces.data[0]);
        Check(triCount == 2, "ufbx_triangulate_face returns 2");
    }
    Check(scene->materials.count == 1, "one material from the .mtl");

    ufbx_free_scene(scene);
}

// ── glTF, through tinygltf v3 ─────────────────────────────────────────────────
// A single triangle with an EXTERNAL .bin, which is the case that proves the filesystem
//	callbacks are wired: without them the buffer never loads and every accessor reads zero.

void TestGLTF (const fs::path& dir)
{
    std::printf("glTF (tinygltf v3)\n");

    // 3 positions (VEC3 float) followed by 3 indices (USHORT), padded to 4 bytes.
    struct Bin {
        float    positions[9];
        uint16_t indices[3];
        uint16_t padding;
    } bin = {
        { 0.0f, 0.0f, 0.0f,
          1.0f, 0.0f, 0.0f,
          0.0f, 1.0f, 0.0f },
        { 0, 1, 2 },
        0
    };
    Check(WriteBinary(dir / "tri.bin", &bin, sizeof(bin)), "write tri.bin");

    const std::string json =
        "{\n"
        "  \"asset\": { \"version\": \"2.0\" },\n"
        "  \"scene\": 0,\n"
        "  \"scenes\": [ { \"nodes\": [0] } ],\n"
        "  \"nodes\": [ { \"mesh\": 0, \"translation\": [1, 2, 3] } ],\n"
        "  \"meshes\": [ { \"primitives\": [ "
        "{ \"attributes\": { \"POSITION\": 0 }, \"indices\": 1, \"material\": 0 } ] } ],\n"
        "  \"materials\": [ { \"pbrMetallicRoughness\": "
        "{ \"baseColorFactor\": [1, 0, 0, 1], \"metallicFactor\": 0.25, "
        "\"roughnessFactor\": 0.75 } } ],\n"
        "  \"buffers\": [ { \"uri\": \"tri.bin\", \"byteLength\": 44 } ],\n"
        "  \"bufferViews\": [\n"
        "    { \"buffer\": 0, \"byteOffset\": 0,  \"byteLength\": 36 },\n"
        "    { \"buffer\": 0, \"byteOffset\": 36, \"byteLength\": 6 }\n"
        "  ],\n"
        "  \"accessors\": [\n"
        "    { \"bufferView\": 0, \"componentType\": 5126, \"count\": 3, \"type\": \"VEC3\", "
        "\"min\": [0,0,0], \"max\": [1,1,0] },\n"
        "    { \"bufferView\": 1, \"componentType\": 5123, \"count\": 3, \"type\": \"SCALAR\" }\n"
        "  ]\n"
        "}\n";
    const fs::path gltfPath = dir / "tri.gltf";
    Check(WriteText(gltfPath, json), "write tri.gltf");

    const std::vector<uint8_t> bytes = ReadAll(gltfPath);
    Check(!bytes.empty(), "read tri.gltf back");

    // The same callbacks the importer installs, minus the engine: plain stdio here.
    struct Fs {
        static int32_t Exists (const char* path, uint32_t len, void*) {
            std::error_code ec;
            return fs::exists(fs::path(std::string(path, len)), ec) ? 1 : 0;
        }
        static int32_t Read (uint8_t** out, uint64_t* size, const char* path, uint32_t len, void*) {
            const std::vector<uint8_t> data = ReadAll(fs::path(std::string(path, len)));
            if (data.empty()) return 0;
            uint8_t* copy = new uint8_t[data.size()];
            std::memcpy(copy, data.data(), data.size());
            *out = copy;
            *size = data.size();
            return 1;
        }
        static void Free (uint8_t* data, uint64_t, void*) { delete[] data; }
        static int32_t Size (uint64_t* out, const char* path, uint32_t len, void*) {
            std::error_code ec;
            const auto size = fs::file_size(fs::path(std::string(path, len)), ec);
            if (ec) return 0;
            *out = size;
            return 1;
        }
    };

    tg3_parse_options options;
    tg3_parse_options_init(&options);
    options.images_as_is     = 1;
    options.validate_indices = 1;
    options.fs.file_exists   = &Fs::Exists;
    options.fs.read_file     = &Fs::Read;
    options.fs.free_file     = &Fs::Free;
    options.fs.get_file_size = &Fs::Size;

    tg3_model model;
    std::memset(&model, 0, sizeof(model));
    tg3_error_stack errors;
    tg3_error_stack_init(&errors);

    const std::string baseDir = dir.string() + "/";
    const tg3_error_code code = tg3_parse_auto(&model, &errors, bytes.data(), bytes.size(),
        baseDir.c_str(), (uint32_t)baseDir.size(), &options);

    if (code != TG3_OK || tg3_errors_has_error(&errors)) {
        const char* message = (errors.count > 0 && errors.entries[0].message != nullptr)
            ? errors.entries[0].message : "(no message)";
        std::printf("  FAIL  tg3_parse_auto: code %d, %s\n", (int)code, message);
        ++g_failures;
        tg3_model_free(&model);
        return;
    }
    std::printf("  ok    tg3_parse_auto\n");

    Check(model.meshes_count == 1, "one mesh");
    Check(model.nodes_count == 1, "one node");
    Check(model.materials_count == 1, "one material");
    Check(model.accessors_count == 2, "two accessors");

    // The external buffer resolved through the callbacks. This is the assertion the whole
    //	test exists for: everything else parses out of the JSON alone.
    Check(model.buffers_count == 1 && model.buffers[0].data.data != nullptr &&
          model.buffers[0].data.count >= 42, "external .bin loaded via fs callbacks");

    Check(tg3_num_components(model.accessors[0].type) == 3, "POSITION is VEC3");
    Check(tg3_component_size(model.accessors[0].component_type) == 4, "POSITION is float");
    Check(tg3_num_components(model.accessors[1].type) == 1, "indices are SCALAR");
    Check(tg3_component_size(model.accessors[1].component_type) == 2, "indices are ushort");

    if (model.buffers_count == 1 && model.buffers[0].data.data != nullptr &&
        model.buffers[0].data.count >= 42) {
        // Read position[1] the way the importer does, straight out of the buffer view.
        const tg3_buffer_view& bv = model.buffer_views[0];
        const uint8_t* base = model.buffers[bv.buffer].data.data + bv.byte_offset;
        float x = 0.0f;
        std::memcpy(&x, base + 3 * sizeof(float), sizeof(float));
        Check(x == 1.0f, "second vertex reads back as x = 1");
    }

    if (model.nodes_count == 1) {
        const tg3_node& node = model.nodes[0];
        Check(node.mesh == 0, "node points at mesh 0");
        Check(node.translation[0] == 1.0 && node.translation[1] == 2.0 &&
              node.translation[2] == 3.0, "node translation parsed");
        Check(node.has_matrix == 0, "node has TRS, not a matrix");
    }

    if (model.materials_count == 1) {
        const tg3_pbr_metallic_roughness& pbr = model.materials[0].pbr_metallic_roughness;
        Check(pbr.base_color_factor[0] == 1.0 && pbr.base_color_factor[1] == 0.0,
            "base colour factor parsed");
        Check(pbr.metallic_factor == 0.25 && pbr.roughness_factor == 0.75,
            "metallic / roughness factors parsed");
    }

    tg3_model_free(&model);
}

} // namespace

int main ()
{
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "simtary_model_import_test";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) {
        std::printf("cannot create temp directory\n");
        return 1;
    }
    std::printf("temp dir: %s\n\n", dir.string().c_str());

    TestOBJ(dir);
    std::printf("\n");
    TestGLTF(dir);

    fs::remove_all(dir, ec);

    std::printf("\n%s\n", g_failures == 0 ? "all model import checks passed"
                                          : "MODEL IMPORT CHECKS FAILED");
    return g_failures == 0 ? 0 : 1;
}
