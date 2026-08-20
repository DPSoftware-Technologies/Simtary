// Generates a sample player animation descriptor (NBT) at
// assets/contents/animation_descriptor/player.staod (override path via argv[1]).
// Run from the repo root after building the `make_player_descriptor` target.
// Edit the result in NBTExplorer, or tweak this generator and re-run.
#include "io/Nbt.h"

#include <cstdio>
#include <string>

using namespace mi::nbt;

static Tag makeState(int id, bool loop, const char* clip) {
    Tag c = Tag::Compound();
    c.put("id", Tag::Short((int16_t)id));
    c.put("loop", Tag::Short(loop ? 1 : 0));
    c.putString("name", clip);
    return c;
}
static Tag makeTransition(const char* from, const char* to, float blend) {
    Tag c = Tag::Compound();
    c.putFloat("blend", blend);
    c.putString("from", from);
    c.putString("to", to);
    return c;
}

int main(int argc, char** argv) {
    Tag root = Tag::Compound();

    Tag anims = Tag::Compound();
    anims.put("state_idle", makeState(0, true, "Idle"));
    anims.put("state_walk", makeState(1, true, "Walking"));
    anims.put("state_run",  makeState(2, true, "Running"));
    root.put("animations", std::move(anims));

    root.put("anitype", Tag::Short(1)); // 1 = player animation
    root.putFloat("default_blend", 0.25f);
    root.putString("player_model_target", "soldier.fbx");

    Tag transitions = Tag::List(Type::Compound);
    transitions.add(makeTransition("state_idle", "state_run",  0.25f));
    transitions.add(makeTransition("state_run",  "state_idle", 0.25f));
    transitions.add(makeTransition("state_idle", "state_walk", 0.25f));
    transitions.add(makeTransition("state_walk", "state_idle", 0.25f));
    transitions.add(makeTransition("state_walk", "state_run",  0.25f));
    transitions.add(makeTransition("state_run",  "state_walk", 0.25f));
    root.put("transitions", std::move(transitions));

    const std::string out = (argc > 1) ? argv[1] : "assets/contents/animation_descriptor/player.staod";
    if (!writeFile(out, root, "animation_descriptor")) {
        std::printf("FAILED to write %s (does the folder exist?)\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s\n", out.c_str());
    std::printf("%s\n", dump(root, "animation_descriptor").c_str());
    return 0;
}
