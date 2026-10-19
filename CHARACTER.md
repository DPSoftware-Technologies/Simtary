# Characters - plan

Status (2026-10-19): **a soldier walks and runs in the engine.** `Milistry/tools/character/` builds a rigged
body with garment parts and the procedural clips idle / walk / run (see its README); the framework's
`stCharacter` (`Framework/character/CharacterComponent.cpp`) drives it on the engine's capsule controller
from the "Player" input map. Not done: appearance record, parts/gear swapping, networking, foot IK, a
better head, crouch / prone / jump clips, a hand-tuned look for the motion.

## Decisions

- Modular characters on ONE shared skeleton (MPFB `game_engine` rig): body + garment / gear parts, each its
  own mesh and material, so the player customises by swapping parts and re-skinning materials.
- Appearance is a small record (macro sliders, part ids per slot, material params, camo pattern + seed),
  saved as NBT and sent at spawn. The server owns the character entity and its appearance; clients build the
  same look from the record (like `st::net` vehicles).
- Assets: MPFB (CC0) + generated parts only. No purchased, scanned, AI-made or credit-requiring asset.
- Framework vs game: the framework gets the component (`stCharacter`: appearance record, assembly from parts,
  locomotion, animation), the game (Milistry) supplies the parts, slots and the creator UI.

## What exists

- `stCharacter` (native component, framework): put it on the root of the imported model next to an
  `stInput` on the "Player" map (Move, Look, Sprint, Jump - a game registers the map; Milistry's
  `OnKeyRegister` does, because it clears the defaults). It adds the engine's `CharacterComponent`
  (capsule, ground, gravity, facing, leaning, clip fading), finds the clips of ITS model by name
  (`idle`, `walk`, `run`: animations whose channels target a descendant), moves relative to the camera,
  chooses the clip by the speed it really moves at and scales the clip rate to match, and with
  `followCamera` runs a simple third-person orbit camera (right mouse orbits, wheel zooms).
- Gotchas: the engine's controller makes an entity face -Z of its own space, so the Blender script turns
  the character 180 degrees before export; full stick / any WASD = the run clip (a partial stick walks);
  the controller collides with scene colliders (it ran straight up onto the HMMWV); the default window
  standby cap (30 fps) applies to an unfocused window as for everything else.
- Clips (`gait.py`): authored in armature space and converted to each bone's rest frame, so they do not
  depend on bone axes; idle 4 s, walk 1 s (1.5 m/s), run 0.67 s (3.7 m/s), seamless loops. Draft quality:
  tune the constants and look at the contact sheets (`mpfb_base.py --sheet`).
- The mesh and the rig had to be lifted onto the ground TOGETHER (the rig is fitted where the mesh is
  created, hips at the origin); `create_human(feet_on_ground=True)` left the bones 0.82 m too low and every
  rotation pivoted about the wrong point.

## Appearance (first version) and the map

- `stCharacter` keeps the player's choices (skin, jacket, trousers, boots, gloves colours, height scale) in the options
  store (`char.*`) and applies them to the model's materials by name (`mat_body`, `mat_top`, ...). **F4** opens the
  customiser (colour pickers, height, Save / Reset). Compiled and wired, NOT yet looked at on screen. It is not a
  network record yet.
- Map `soldiertest` (`Milistry/assets/scenes/soldiertest.stsd`, a folder scene, `--scene=soldiertest`): a 800 x 800 m
  generated terrain (plains + hills, gentle) and the soldier at the origin. No vehicle in it yet.
- Ground: the controller collides with terrain CHUNK meshes, which are built around the camera a while after a load.
  `stCharacter` therefore stands on the authored terrain DATA (`TerrainSystem::Sample`) from the first frame and never
  goes below it; it moves 0.4 s after the scene loads (was ~35 s while it waited for chunks). The ray hold remains
  only for maps without authored terrain.
- **Terrain gave the controller a fake moving platform.** On first contact a just-placed chunk reports its whole
  height as one frame of velocity; the controller stores that as `inertia` (moving-platform carry) and drifted up at
  2.28 m per frame for ever - the soldier flew into the sky (the "floating over the water" bug). Engine fix (SIMTARY,
  `RunCharacterUpdateSystem`): no inertia from objects flagged `FILTER_TERRAIN`; `stCharacter` also clears inertia
  within 1 m of the terrain.
- Clips were exported at Blender's default 24 fps (walk 1.25 s instead of 1.0 s - sliding feet); `gait.py` now sets
  the scene to 30 fps.
- **This fork keeps a 64-bit absolute position as the transform's truth and rebuilds `translation_local` from it each
  update.** Writing `translation_local` alone is undone, and so was the engine's own character write (the entity never
  moved). `stCharacter` writes the entity in `LateUpdate` with `SyncWorldFromLocal(GetRenderOrigin())`, using the
  controller's raw `GetPosition()` - the interpolated one reads a stale previous step while the character stands still.
- Animation without animation files: the clips are generated (gait.py). Physics has a place on top of that - ragdoll on
  death / impact (the engine has a humanoid ragdoll; the rig's bones must be mapped to humanoid slots first), foot IK,
  hit reactions blended over the walk - but walking itself driven by physics (active ragdoll) is fragile; keep the
  procedural gait as the base.

## Next steps

1. Crouch / prone / jump / turn-in-place clips, foot IK (needs a HumanoidComponent mapping of the rig).
2. `stCharacter` assembly: load body + parts from the record, material overrides, spawn through the
   integrated host so single player and multiplayer share the path.
3. Gear parts (helmet, plate carrier, pack) and a head (eyes, hair).
4. Creator screen: sliders, part pickers, camo preview.
5. Camo: pattern generator that reads as modern (digital / multi-scale), applied through the material.
6. Networking: `CharacterSync` (input up, snapshots down, appearance at spawn).
