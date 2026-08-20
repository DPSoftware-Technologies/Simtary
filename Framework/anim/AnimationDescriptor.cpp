#include "anim/AnimationDescriptor.h"
#include "io/Nbt.h"

namespace st::anim {

const AnimState* AnimationDescriptor::findState(const std::string& name) const {
    for (const AnimState& s : states)
        if (s.state == name) return &s;
    return nullptr;
}
const AnimTransition* AnimationDescriptor::findTransition(const std::string& from, const std::string& to) const {
    for (const AnimTransition& t : transitions)
        if (t.from == from && t.to == to) return &t;
    return nullptr;
}

bool LoadAnimationDescriptor(const std::string& path, AnimationDescriptor& out, std::string* error) {
    nbt::Tag root;
    if (!nbt::readFile(path, root, nullptr, error))
        return false;

    out = AnimationDescriptor();
    out.anitype      = root.getInt("anitype", 0);
    out.modelTarget  = root.getString("player_model_target");
    out.defaultBlend = root.getFloat("default_blend", 0.25f);

    // animations: compound of <stateName> -> compound{ id, loop, name }
    if (const nbt::Tag* anims = root.get("animations")) {
        for (const auto& kv : anims->children) {
            const nbt::Tag& c = kv.second;
            AnimState s;
            s.state = kv.first;
            s.clip  = c.getString("name");
            s.id    = c.getInt("id", 0);
            s.loop  = c.getBool("loop", true);
            out.states.push_back(std::move(s));
        }
    }

    // transitions: list of compound{ from, to, blend }
    if (const nbt::Tag* trans = root.get("transitions")) {
        for (const nbt::Tag& it : trans->items) {
            AnimTransition t;
            t.from  = it.getString("from");
            t.to    = it.getString("to");
            t.blend = it.getFloat("blend", out.defaultBlend);
            out.transitions.push_back(std::move(t));
        }
    }

    if (out.states.empty()) {
        if (error) *error = "animation descriptor has no animations: " + path;
        return false;
    }
    return true;
}

} // namespace st::anim
