# Darktide asset compiler (beta)

Blender addon + compiler for making your own models, props and gear for darktide. You do the work in blender, hit compile, and you get native darktide resources (unit, materials, textures, bones, animations, physics) plus a `build.json`.

Turning that output into something the game loads is done by the custom assets patcher, which is its own thing:
https://github.com/fviuff/darktide-mods/tree/main/Custom-Assets-patcher

This is a beta. Static props, materials, collision, skinned stuff that follows a character and simple animations work in game. Some things don't yet, see known problems at the bottom.

## Setup

Grab the release zip for your platform. It has `DarktideGLBCompiler.exe`, `darktide_assets_blender.zip` and this readme.

In blender (made and tested on 5.2) go to edit > preferences > add-ons > install from disk, pick `darktide_assets_blender.zip` and enable it. If you had an older version installed, remove it first and restart blender. Press N in the 3d view and open the darktide tab.

In the build box set the compiler to `DarktideGLBCompiler.exe` and the output path to some working folder. Save your .blend somewhere first.

Textures get compressed with oodle, and the compiler borrows the dll from your darktide install. It finds the normal steam location by itself. If your game is somewhere else, run this once from the sdk folder:

```
DarktideGLBCompiler.exe --configure-oodle "D:\Games\Warhammer 40,000 DARKTIDE"
```

## Your first prop

1. Make something simple, the default cube is fine. Apply scale (ctrl+a > scale).
2. Give it a material. A normal principled bsdf with a color and roughness is enough.
3. Select it and click "New Asset from Selection". That makes an asset collection, one collection per asset.
4. Set the collection's resource path to something like `content/mods/my_mod/props/test_crate`. Lowercase only, keep it the same every time you rebuild, and use a different one for every asset.
5. Click "Inspect Asset" if you want a quick check of what will be exported, then "Compile Asset".

You get a folder named after the asset in your output path with the unit, materials, textures, `build.json` and `build.log`. If something fails, `build.log` is the first place to look.

To get it in game, drop that folder into `mods/<YourMod>/Custom/<folder>/`, run the patcher with the game closed, and spawn or use it from your mod through the custom assets api.

## Materials

The compiler reads the normal gltf style pbr setup: base color, metallic, roughness, alpha, normal map and emission, with or without image textures. Hook normal maps up through a normal map node, color images as srgb, the rest as non-color. Random shader node setups don't translate, keep it simple.

A single emission shader plugged straight into the output makes a cheap self lit material with no textures needed.

If you want to use a material that already exists in the game (or in your mod), set the material intent to use external material and type its resource path.

Texture transforms: offsets, 90 degree turns and flips are fine, fractional scaling and odd rotations are not.

## Collision and physics

For a plain solid prop turn on use visible meshes as collision and leave body behavior on fixed collider. For cleaner collision, "Create Editable Collision Mesh" makes a separate collision-only copy you can simplify.

Body behavior options:
- fixed collider: doesn't move, players and everything else bump into it
- simulated body: real physics, falls and gets pushed around. Uses a convex hull, so concave stuff needs a few separate convex pieces
- moved externally: kinematic, your code moves it

Heads up: simulated bodies collide with the level and props but not with players. That's how the game's own loose props work too. If players need to bump into it (like a door), make it animated with fixed or moved externally collision instead.

For things made of several parts, turn on body on each part object, give each one a collider, and connect them with an empty set to fixed, hinge or ragdoll under joint (body a / body b). A hinge turns around the empty's x axis, so point that along the hinge.

A spawned simulated prop can sit there asleep until something touches it. `Actor.wake_up(actor)` sorts that out.

## Animation

Make an action per animation. If you want more than one, push each finished action down to its own nla track (dope sheet > action editor > push down) and name them something useful. A plain animated object works too, you don't need to rig a spinning fan.

By default the first clip gets stored in the unit as a simple animation (the clip field next to the simple animation checkbox picks a different one). Nothing plays by itself, your mod starts it:

```lua
Unit.play_simple_animation(unit, 0, nil, true, 1) -- from, to (nil = end), loop, speed
```

That's the same thing the game uses for magazines, levers and a bunch of props.

There's also a looping single clip state machine (auto-loop single-clip assets, on by default when the glb has exactly one clip). That one works but you still have to switch it on from lua, the game never starts a state machine on its own:

```lua
Unit.enable_animation_state_machine(unit)
```

The event driven state graph options are in the addon but don't work in game yet, see known problems.

## Rigging to a darktide skeleton

Anything that should move with a character (gloves, a helmet, a whole new body) has to use that character's skeleton. The addon reads the skeleton from the game files, so you need the game resources extracted once into a folder (limn with raw dump works) with the `<hash>.unit` / `<hash>.bones` files in it.

In the asset collection's reference skeleton box set the game extract folder to that folder and pick a preset: human, ogryn player, traitor guard (most renegade enemies) or flamer. Custom lets you point at any other unit/bones pair.

Easiest way to start is "New Starter Asset from Skeleton". Pick a skeleton and hands, head or full body proxy and you get the real armature with a simple mesh already weighted to the right bones. Reshape it into whatever you want and compile. "Import Rest-Pose Reference Skeleton" gives you just the armature if you'd rather model from scratch.

### Using a model you already have

A model rigged somewhere else has different bone names, different proportions and usually a pile of extra bones (cloth, jiggle, holders and so on). The addon can convert it:

1. Import it, select the armature and meshes, "New Asset from Selection".
2. Set the extract folder and preset like above.
3. Click "Auto-map Bones". It pairs your bones with darktide bones from their names and the rig structure. Go through the list anyway. Hips, spine, neck, head, shoulders, arms, hands, fingers, legs and feet should all be paired. Fix wrong ones with the search field. Leave cloth/jiggle/weapon/helper bones empty, they get kept as extra bones that just follow their parent.
4. Click "Fit to Darktide Skeleton". It scales and poses the model onto the darktide rest pose, moves the meshes and shape keys with it, skins stuff that was just parented to a bone, and swaps in a new armature with the darktide names plus your extras. The old one gets hidden. Read the little report it gives you.
5. Pose a few bones to check it looks right, then compile. Only weighted bones (gear) gets switched on after a fit so only bones your meshes actually use end up in the build.

The model ends up with the darktide skeleton's proportions, since the game's animations drive those bones.

### Putting it on a character

Link it the same way the game attaches its own gear, root to root with node name mapping, so every bone follows the character's bone with the same name:

```lua
local unit = World.spawn_unit_ex(world, resource_name, nil, Unit.world_pose(character, 1))
World.link_unit(world, unit, 1, character, 1, World.LINK_MODE_NODE_NAME)
```

If it replaces the character's look, hide the original with `Unit.set_unit_objects_visibility`. The game's own gear hiding goes through item data which this doesn't touch.

## Linux

The linux zip is the same windows exe, the addon runs it through wine and converts the paths for you. You need wine and winepath installed. For textures point the compiler at your game's oodle dll once, using the same wine prefix you run blender with:

```
wine ./DarktideGLBCompiler.exe --configure-oodle "$(winepath -w "$HOME/.local/share/Steam/steamapps/common/Warhammer 40,000 DARKTIDE")"
```

I haven't tested this, I asked a llm, dont trust me.

## Known problems

- Event driven state graphs (multiple states, events, variables) compile but the game doesn't pick them up yet, the unit ends up with no state machine. Simple animation and the single looping state machine work.
- Ragdolls: the bodies and joints get written, but switching a character from animation over to ragdoll isn't something you can set up yet.
- Shape keys get baked, you can't animate them in game.
- Only one uv map really gets used for the textures in a material.
- Jiggle and cloth bones don't simulate, they just follow their parent.
- Auto-map needs an actual head bone. On rigs without one, check the head row before fitting.
- Gltf extras like clearcoat, sheen and transmission are approximated with plain pbr.

## Common stuff

- Compiler not found: set the compiler in the build box to the exe. On linux also check wine and winepath.
- A member is missing or excluded: the collection has to be enabled in the view layer and its objects visible and selectable.
- Something missing from the output: check the collection members, then compare the inspect report and `build.log`.
- Texture or oodle errors: see setup, run `--configure-oodle` once.
- My animated thing just stands there: nothing plays automatically, call `Unit.play_simple_animation` or enable the state machine from your mod.

Keep your .blend and source images around, the build output doesn't include them. One output folder per resource path.

Third party licenses are in `licenses/` and `THIRD_PARTY_LICENSES.txt`.
