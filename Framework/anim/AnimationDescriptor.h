#pragma once
// Animation descriptor: a data-driven mapping (stored as NBT, see src/io/Nbt) that tells the
// engine which animation clips a character uses, how they loop, and the state-machine transitions
// between them. One descriptor file per character/thing lives under assets/animation_descriptor/.
//
// It is a PACKAGED asset, not a loose file. `.staod` goes into the .strd/.stafp package with
// everything else under assets/contents/ (AssetType::Animation), and LoadAnimationDescriptor
// reads it through wi::helper::FileRead - the same seam st::AssetSystem overrides for textures
// and scenes - so the path "assets/animation_descriptor/player.staod" resolves out of a mounted
// package. Nothing in the caller changes: a package that does not hold the path falls through to
// the real filesystem, so a loose descriptor dropped in during development still wins nothing
// and loses nothing.
//
// File layout (NBT compound), matching what NBTExplorer shows:
//   anitype             : short   - descriptor kind. 1 = player animation.
//   player_model_target : string  - name of the model node under the rig (e.g. "soldier.fbx")
//   default_blend       : float   - crossfade seconds used when a transition has none
//   animations          : compound{ <stateName> : compound{ id:short, loop:short, name:string } }
//                                   `name` is the engine animation clip entity name (e.g. "Idle").
//   transitions         : list<compound{ from:string, to:string, blend:float }>  - the state graph
//
// The descriptor only maps states→clips and the allowed transitions; WHAT triggers a transition is
// gameplay logic (for anitype 1 the player animator derives idle/walk/run from movement).

#include <string>
#include <vector>

namespace st::anim {

struct AnimState {
    std::string state;       // state-machine key, e.g. "state_idle"
    std::string clip;        // engine animation clip entity name, e.g. "Idle"
    int  id   = 0;           // author-assigned numeric id for the state
    bool loop = true;
};

struct AnimTransition {
    std::string from;
    std::string to;
    float blend = 0.25f;     // crossfade seconds
};

struct AnimationDescriptor {
    int         anitype     = 0;
    std::string modelTarget;
    float       defaultBlend = 0.25f;
    std::vector<AnimState>      states;       // insertion order from the file
    std::vector<AnimTransition> transitions;

    const AnimState*      findState(const std::string& name) const;
    const AnimTransition* findTransition(const std::string& from, const std::string& to) const;
};

// Load + parse an NBT descriptor file. Returns false (and sets *error) on missing file or bad data.
bool LoadAnimationDescriptor(const std::string& path, AnimationDescriptor& out, std::string* error = nullptr);

} // namespace st::anim
