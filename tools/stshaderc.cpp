// stshaderc - compile one HLSL file to DXIL or SPIR-V through the ENGINE's shader
// compiler, for the framework's own shaders (ImGui, lens flare, projector, laser).
//
// It exists because dxc.exe is not a dependable SPIR-V compiler on Windows: the copy
// that ships with the Windows 10/11 SDK is DXIL only ("SPIR-V CodeGen not available"),
// so a machine without the Vulkan SDK could not build the framework's shaders for a
// Vulkan run - and on Windows this workspace can start on either backend
// (Framework/render/GraphicsAPI.h). The dxcompiler.dll vendored in Engine/ does both,
// and wi::shadercompiler is what already drives it for the engine's own ~360 shaders.
//
// Using that compiler also means the DX12 root signature and the Vulkan binding shifts
// come from GraphicsDevice_Vulkan's own constants rather than from a copy of them
// pasted into CMake, so the two can no longer drift apart.
//
//   stshaderc -T ps_6_0 -E main -F spirv [-I dir]... [-D NAME=VALUE]...
//             [-Od] [-strip] source.hlsl -Fo out.cso
//
// The flag names follow dxc's, since that is what this replaced in
// cmake/SimtaryApp.cmake and what anyone reading the build log will expect.

#include "wiShaderCompiler.h"
#include "wiBacklog.h"
#include "wiHelper.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace wi::graphics;

namespace {

// "ps_6_0" -> ShaderStage::PS + ShaderModel::SM_6_0. dxc's own profile spelling, so a
// build command reads the same as the dxc one it replaced.
bool ParseProfile (const std::string& profile, ShaderStage& stage, ShaderModel& model) {
    const size_t split = profile.find('_');
    if (split == std::string::npos)
        return false;

    const std::string stageName = profile.substr(0, split);
    const std::string modelName = profile.substr(split + 1);

    if      (stageName == "vs")  stage = ShaderStage::VS;
    else if (stageName == "ps")  stage = ShaderStage::PS;
    else if (stageName == "cs")  stage = ShaderStage::CS;
    else if (stageName == "gs")  stage = ShaderStage::GS;
    else if (stageName == "hs")  stage = ShaderStage::HS;
    else if (stageName == "ds")  stage = ShaderStage::DS;
    else if (stageName == "as")  stage = ShaderStage::AS;
    else if (stageName == "ms")  stage = ShaderStage::MS;
    else if (stageName == "lib") stage = ShaderStage::LIB;
    else return false;

    if      (modelName == "5_0") model = ShaderModel::SM_5_0;
    else if (modelName == "6_0") model = ShaderModel::SM_6_0;
    else if (modelName == "6_1") model = ShaderModel::SM_6_1;
    else if (modelName == "6_2") model = ShaderModel::SM_6_2;
    else if (modelName == "6_3") model = ShaderModel::SM_6_3;
    else if (modelName == "6_4") model = ShaderModel::SM_6_4;
    else if (modelName == "6_5") model = ShaderModel::SM_6_5;
    else if (modelName == "6_6") model = ShaderModel::SM_6_6;
    else if (modelName == "6_7") model = ShaderModel::SM_6_7;
    else return false;

    return true;
}

void PrintUsage () {
    printf("stshaderc -T <profile> -E <entry> -F <hlsl6|spirv> [-I <dir>]... "
           "[-D <NAME[=VALUE]>]... [-Od] [-strip] <source> -Fo <output>\n");
}

// The next argument of a flag that takes one, or nullptr when it is missing.
const char* Value (int argc, char* argv[], int& i) {
    if (i + 1 >= argc)
        return nullptr;
    return argv[++i];
}

} // namespace

int main (int argc, char* argv[]) {
    wi::shadercompiler::CompilerInput input;
    input.format = ShaderFormat::HLSL6;
    std::string source;
    std::string output;
    bool haveProfile = false;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];

        if (strcmp(arg, "-T") == 0) {
            const char* profile = Value(argc, argv, i);
            if (profile == nullptr || !ParseProfile(profile, input.stage, input.minshadermodel)) {
                printf("stshaderc: unknown or missing profile after -T\n");
                return 1;
            }
            haveProfile = true;
        } else if (strcmp(arg, "-E") == 0) {
            const char* entry = Value(argc, argv, i);
            if (entry == nullptr) { PrintUsage(); return 1; }
            input.entrypoint = entry;
        } else if (strcmp(arg, "-F") == 0) {
            const char* format = Value(argc, argv, i);
            if (format == nullptr) { PrintUsage(); return 1; }
            if      (strcmp(format, "hlsl6") == 0) input.format = ShaderFormat::HLSL6;
            else if (strcmp(format, "spirv") == 0) input.format = ShaderFormat::SPIRV;
            else {
                printf("stshaderc: unknown format '%s' (expected hlsl6 or spirv)\n", format);
                return 1;
            }
        } else if (strcmp(arg, "-I") == 0) {
            const char* dir = Value(argc, argv, i);
            if (dir == nullptr) { PrintUsage(); return 1; }
            input.include_directories.push_back(dir);
        } else if (strcmp(arg, "-D") == 0) {
            const char* define = Value(argc, argv, i);
            if (define == nullptr) { PrintUsage(); return 1; }
            input.defines.push_back(define);
        } else if (strcmp(arg, "-Fo") == 0) {
            const char* file = Value(argc, argv, i);
            if (file == nullptr) { PrintUsage(); return 1; }
            output = file;
        } else if (strcmp(arg, "-Od") == 0) {
            input.flags |= wi::shadercompiler::Flags::DISABLE_OPTIMIZATION;
        } else if (strcmp(arg, "-strip") == 0) {
            input.flags |= wi::shadercompiler::Flags::STRIP_REFLECTION;
        } else if (arg[0] == '-') {
            printf("stshaderc: unknown argument '%s'\n", arg);
            PrintUsage();
            return 1;
        } else {
            source = arg;
        }
    }

    if (!haveProfile || source.empty() || output.empty()) {
        PrintUsage();
        return 1;
    }

    input.shadersourcefilename = source;

    // The source file's own directory, so a quoted include of a sibling
    // ("StLensFlare.hlsli") resolves the way it does under dxc.exe. It does not come
    // for free here: wi::shadercompiler reads the file itself and hands dxc a BUFFER,
    // so dxc has no idea where the source came from and resolves a relative include
    // against the working directory instead. Added last, so an explicit -I still wins.
    const std::string sourceDirectory = wi::helper::GetDirectoryFromPath(source);
    if (!sourceDirectory.empty())
        input.include_directories.push_back(sourceDirectory);

    wi::shadercompiler::CompilerOutput result;
    wi::shadercompiler::Compile(input, result);

    if (!result.IsValid() || result.shaderdata == nullptr || result.shadersize == 0) {
        // The compiler puts the diagnostic here; the backlog it also posts to has no
        // console behind it in a build step.
        printf("stshaderc: %s failed:\n%s\n", source.c_str(),
               result.error_message.empty() ? "no output was produced" : result.error_message.c_str());
        return 1;
    }
    if (!result.error_message.empty()) {
        // Warnings: the compile succeeded, so this is not a failure - but a build that
        // silently swallows them is how a shader rots.
        printf("stshaderc: %s:\n%s\n", source.c_str(), result.error_message.c_str());
    }

    if (!wi::helper::FileWrite(output, result.shaderdata, result.shadersize)) {
        printf("stshaderc: could not write %s\n", output.c_str());
        return 1;
    }
    return 0;
}
