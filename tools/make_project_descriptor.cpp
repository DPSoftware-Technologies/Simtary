// make_project_descriptor — author a Simtary project descriptor (.stpd).
//
// The descriptor is the project's BUILD-TIME manifest, the equivalent of Unreal's
// .uproject: who the project is, not how it behaves. It lives at
// <project>/assets/project.stpd and is read by CMake at configure time — never at
// runtime, and never shipped, which is why it sits in assets/ rather than
// assets/contents/ (that folder is the built game's content).
//
// Runtime-tunable properties (window size, startup scene, DevUI mode) deliberately do
// NOT live here: baking them in would mean a rebuild to change them. They stay in
// st::AppConfig in src/main.cpp, and user-facing ones end up in options.stad.
//
// Being NBT it is binary, so this is the tool that writes it. It EDITS rather than
// replaces: an existing file is read first and only the flags you pass are changed,
// so regenerating never drops keys the build does not know about yet.
//
//   make_project_descriptor assets/project.stpd --name "My Game" --version 1.2.0
//   make_project_descriptor assets/project.stpd --dump
//   make_project_descriptor assets/project.stpd --cmake     (consumed by the build)
//
// See Simtary/cmake/SimtaryProject.cmake for how the build reads it.

#include "io/Nbt.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using st::nbt::Tag;

namespace {

// Must match st::PROJECT_ROOT_NAME conceptually; the runtime no longer reads this
// file, so the name only has to agree with SimtaryProject.cmake.
constexpr const char* kRootName = "project";

void Usage() {
    printf(
        "usage: make_project_descriptor <file.stpd> [options]\n"
        "\n"
        "  --dump                      print the descriptor and exit\n"
        "  --cmake                     print set() lines for the build and exit\n"
        "\n"
        "  [project]  identity\n"
        "  --name <text>               display name: window title, About box, exe metadata\n"
        "  --organization <text>       user data folder: LocalLow/<org>/<name>/\n"
        "  --copyright <text>          shown in the About window and the exe metadata\n"
        "  --version <x.y.z>           product version baked into the exe\n"
        "\n"
        "  [build]\n"
        "  --icon <path>               .ico compiled into the executable\n"
        "  --target-name <text>        CMake target / exe filename (defaults to --name)\n"
        "\n"
        "Only the flags you pass are changed; everything else in the file is kept.\n"
        "Runtime properties (window size, startup scene, DevUI mode) are NOT here --\n"
        "they live in st::AppConfig in src/main.cpp.\n");
}

Tag& Section(Tag& root, const char* name) {
    if (Tag* existing = root.get(name))
        if (existing->isCompound())
            return *existing;
    return root.put(name, Tag::Compound());
}

// CMake has no string escape for embedded quotes/backslashes in a set(), so escape.
std::string Escape(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

void EmitCMake(const Tag& root, const char* section, const char* key, const char* var) {
    const Tag* s = root.get(section);
    if (s == nullptr || !s->has(key))
        return;
    printf("set(%s \"%s\")\n", var, Escape(s->getString(key)).c_str());
}

bool NeedsValue(int i, int argc, const char* flag) {
    if (i + 1 < argc)
        return true;
    fprintf(stderr, "error: %s needs a value\n", flag);
    return false;
}

} // namespace

int main (int argc, char* argv[]) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        Usage();
        return argc < 2 ? 1 : 0;
    }

    const std::string path = argv[1];

    Tag root;
    std::string error;
    const bool existed = st::nbt::readFile(path, root, nullptr, &error);
    if (!existed || !root.isCompound())
        root = Tag::Compound();

    bool dump = false;
    bool cmake = false;
    bool changed = false;

    for (int i = 2; i < argc; ++i) {
        const char* a = argv[i];
        auto text = [&](const char* flag, const char* section, const char* key) {
            if (strcmp(a, flag) != 0) return false;
            if (!NeedsValue(i, argc, flag)) exit(1);
            Section(root, section).putString(key, argv[++i]);
            changed = true;
            return true;
        };

        if (strcmp(a, "--dump")  == 0) { dump  = true; continue; }
        if (strcmp(a, "--cmake") == 0) { cmake = true; continue; }

        if (text("--name",         "project", "name"))         continue;
        if (text("--organization", "project", "organization")) continue;
        if (text("--copyright",    "project", "copyright"))    continue;
        if (text("--version",      "project", "version"))      continue;
        if (text("--icon",         "build",   "icon"))         continue;
        if (text("--target-name",  "build",   "target_name"))  continue;

        fprintf(stderr, "error: unknown option '%s'\n\n", a);
        Usage();
        return 1;
    }

    if (cmake) {
        // Consumed by SimtaryProject.cmake via include(). Stay silent on stdout apart
        // from set() lines -- anything else becomes a CMake syntax error.
        if (!existed) {
            fprintf(stderr, "error: '%s' does not exist (%s)\n", path.c_str(), error.c_str());
            return 1;
        }
        EmitCMake(root, "project", "name",         "ST_PROJECT_NAME");
        EmitCMake(root, "project", "organization", "ST_PROJECT_ORGANIZATION");
        EmitCMake(root, "project", "copyright",    "ST_PROJECT_COPYRIGHT");
        EmitCMake(root, "project", "version",      "ST_PROJECT_VERSION");
        EmitCMake(root, "build",   "icon",         "ST_PROJECT_ICON");
        EmitCMake(root, "build",   "target_name",  "ST_PROJECT_TARGET_NAME");
        return 0;
    }

    if (dump) {
        if (!existed) {
            fprintf(stderr, "error: '%s' does not exist (%s)\n", path.c_str(), error.c_str());
            return 1;
        }
        printf("%s\n", st::nbt::dump(root, kRootName).c_str());
        return 0;
    }

    if (!changed && existed) {
        printf("Nothing to change. Pass --dump to inspect, --help for options.\n");
        return 0;
    }

    if (!st::nbt::writeFile(path, root, kRootName)) {
        fprintf(stderr, "error: could not write '%s'\n", path.c_str());
        return 1;
    }

    printf("%s '%s'\n", existed ? "Updated" : "Wrote", path.c_str());
    printf("%s\n", st::nbt::dump(root, kRootName).c_str());
    return 0;
}
