# Darktide asset compiler (beta)

Blender addon + compiler for making your own models, props and gear for darktide. You do the work in blender, hit compile, and you get native darktide resources (unit, materials, textures, bones, animations, physics) plus a `build.json`.

Turning that output into something the game loads is done by the custom assets patcher, which is its own thing:
https://github.com/fviuff/darktide-mods/tree/main/Custom-Assets-patcher

This is a beta. Static props, materials, collision, skinned stuff that follows a character and simple animations work in game. Some things don't yet, see known problems at the bottom.

## Setup

Grab the release zip (same one for windows and linux). It has `DarktideGLBCompiler.exe`, `darktide_assets_blender.zip` and this readme.

In blender (made and tested on 5.2) go to edit > preferences > add-ons > install from disk, pick `darktide_assets_blender.zip` and enable it. If you had an older version installed, remove it first and restart blender. Press N in the 3d view and open the darktide tab.

In the build box set the compiler to `DarktideGLBCompiler.exe` and the output path to some working folder. Save your .blend somewhere first.

Textures get compressed with oodle, and the compiler borrows the dll from your darktide install. It finds the normal steam location (`C:\Program Files (x86)\Steam\steamapps\common\Warhammer 40,000 DARKTIDE`) by itself. If your game is anywhere else (another drive, another steam library) you have to tell it once:

1. Open the start menu, type `cmd` and open command prompt.
2. Go to the folder you unpacked the sdk into. The `/d` is needed when it's on another drive than C:

   ```
   cd /d "D:\path\to\sdk folder"
   ```

3. Run this with your own game folder, the one that has `binaries` and `bundle` in it:

   ```
   DarktideGLBCompiler.exe --configure-oodle "D:\SteamLibrary\steamapps\common\Warhammer 40,000 DARKTIDE"
   ```

Keep the quotes around the paths, they have spaces in them and without quotes it doesn't work. If it worked it says `Oodle configured.` You only do this once, it's remembered. Game shaders also read the game files from this folder.

## Your first prop

1. Make something simple, the default cube is fine. Apply scale (ctrl+a > scale).
2. Give it a material. A normal principled bsdf with a color and roughness is enough.
3. Select it in the 3d view and click "New Asset from Selection". That makes an asset collection, one collection per asset.
4. Set the collection's resource path to something like `content/mods/my_mod/props/test_crate`. Lowercase only, keep it the same every time you rebuild, and use a different one for every asset.
5. Click "Inspect Asset" if you want a quick check of what will be exported, then "Compile Asset".

About selecting: select the objects themselves in the 3d view (drag a box around the whole model, or A for everything). Clicking a collection in the outliner selects nothing, and you end up with an empty asset. For a rigged model select the armature and all its meshes. Children of what you select come along by themselves, and so does the armature a mesh is skinned to. If you already made the asset and forgot something, select everything again and use "Replace Asset Members from Selection".

You get a folder named after the asset in your output path with the unit, materials, textures, `build.json` and `build.log`. If something fails, `build.log` is the first place to look. Inspect and compile also leave a `.glb` in the output path, which is exactly what the addon handed to the compiler. Import it into an empty blender scene if something looks off in game, that's what the compiler saw.

To get it in game, drop that folder into `mods/<YourMod>/Custom/<folder>/`, run the patcher with the game closed, and spawn or use it from your mod through the custom assets api.

## Materials

The compiler reads the normal gltf style pbr setup: base color, metallic, roughness, alpha, normal map and emission, with or without image textures. Hook normal maps up through a normal map node, color images as srgb, the rest as non-color. Random shader node setups don't translate, keep it simple.

A single emission shader plugged straight into the output makes a cheap self lit material with no textures needed.

If you want to use a material that already exists in the game (or in your mod), set the material intent to use external material and type its resource path.

For glass, water, holograms, glowing or pulsing surfaces, crystal, fur and the like, set the intent to Game shader and pick one. It copies one of the game's own materials using that shader. "Values" changes its settings, e.g. `color=1,0.2,0.1; opacity=0.8` (the panel lists what the shader has and its current values). "Textures" puts your images into its texture slots, e.g. `bca=base_color; nm=normal` (orm and emissive work too). Settings without a readable name show up as `#1234abcd`, and you can set those the same way. It reads the game's files, so point "Darktide folder" at your install if it isn't found. The shader itself is copied into your asset too, so it works anywhere, not just where the game happens to have it loaded. Unlit color draws its color as raw brightness: around 0.1 per channel is a full color, 1 and up blows out to white.

Texture transforms: offsets, 90 degree turns and flips are fine, fractional scaling and odd rotations are not.

## Lights

Point and spot lamps in the collection become lights on the unit and move with whatever they're parented to. Brightness is blender watts times 60, so a freshly added 10 W lamp is a normal game lamp (600). Color comes straight from the lamp, the range is 8 m unless the light has a custom distance, and they don't cast shadows yet. Sun lamps are skipped. `Unit.light(unit, index)` plus the `Light.*` functions change them at runtime.

## Visibility groups

Type a name into "Visibility group" on an object and its meshes, plus every mesh parented under it, can be hidden and shown together: `Unit.set_visibility(unit, "name", false)`. Handy for variants, a helmet visor, a lid, that kind of thing. An empty works fine as the group holder.

## Dangling bits

Straps, cables, pouches, tassels: select a bone, open bone properties and tick "Darktide Dangle". It then swings under gravity in game like the dangly bits on the game's own gear. Skin the swinging mesh to the bone as usual. A chain of dangle bones makes a floppy strap. Length (0 = the bone's length), damping, stiffness (pull back to rest) and max angle tune it.

Switch the mode to Jiggle for things that should bounce in place instead of swing (pouches, bellies, antennae): the bone's position springs behind where the animation puts it. Stiffness, damping and max stretch tune it.

It runs inside the unit's animation state machine, so your mod needs `Unit.enable_animation_state_machine(unit)` after spawning (units without animations get a still state made for them). There is no real cloth simulation in the game; this is what it uses instead.

## Particle effects

Particle effects live on an empty. You can build one from scratch or start from any of the game's own effects (fire, sparks, smoke, steam, muzzle flashes...) and change whatever you like. The materials, shaders and textures an effect draws with come along as copies, so it works anywhere your asset is loaded.

1. Extract the game files with limn like in the rigging section, but with `particles material texture unit` at the end instead (you can extract into the same folder). Set the game extract folder and the compiler path.
2. Add an empty to your asset: mouse over the 3d view, shift+a > empty > plain axes. In the properties editor, object tab (the orange square), tick "Darktide Particle Effect".
3. Either click the + next to the systems list for a new system (small sparks rising from a point), or type a game effect's resource path, e.g. `content/fx/particles/environment/brazier_01`, and click "Import".
4. Edit it. It works like the Stingray particle editor:
   - an effect has one or more systems, each with its own max particles
   - channels are the numbers every particle carries (position, velocity, age, life, size...). 4 bytes is one number, 16 is a vector
   - initializers run when a particle is born: where it spawns (position sphere/box/cylinder), how fast it goes (velocity cone/box), random sizes and lifetimes (random float)...
   - simulators run every frame: emitters (rate emitter = particles per second, burst emitter = a bunch at once), age age (ages particles and kills them when their life is up), velocity accelerate (gravity), position integrate (moves them)...
   - visualizers draw them. A billboard is a camera facing sprite with a material. Its vertex writers hand the particle's data to the material's shader: size over life (curve), color (opacity curve + color ramp), alignment to velocity...
   - curves are normal blender curves and colors are color ramps. The game draws straight lines between the points, up to 10 of them
   - the type button on each item swaps it for another type, the arrows reorder, x removes
5. Click "Load" under materials to see every material the effect draws with, its current values and its texture channels. Change what you want, the same way as game shader materials: "Values" like `#423d7a88=0.04,1,0,1` (names the addon can't read show as `#1234abcd`, they work all the same), "Textures" like `diffuse_map=//my_spark.png`.
6. Compile. The effect gets the resource name you see on the empty, in your asset's folder.

Heads up: a billboard's vertex channels have to match what its material's shader reads, so when you swap the material, take the vertex channels and writers from a game effect that uses that material (import it on a spare empty and copy them over).

From your mod, once the asset is loaded:

```lua
local id = World.create_particles(world, "content/mods/my_mod/green_fire", position)
World.link_particles(world, id, unit, node_index, Matrix4x4.identity(), "stop") -- optional, follow a unit
World.destroy_particles(world, id)
```

Effects that draw game models as particles (debris, shell casings and the like) bring copies of those models along. A few whose models need more than materials (bones, animation) can't be copied yet.

## Collision and physics

For a plain solid prop turn on use visible meshes as collision and leave body behavior on fixed collider. For cleaner collision, "Create Editable Collision Mesh" makes a separate collision-only copy you can simplify.

Body behavior options:
- fixed collider: doesn't move, players and everything else bump into it
- simulated body: real physics, falls and gets pushed around. Uses a convex hull, so concave stuff needs a few separate convex pieces
- moved externally: kinematic, your code moves it

Heads up: simulated bodies collide with the level and props but not with players. That's how the game's own loose props work too. If players need to bump into it (like a door), make it animated with fixed or moved externally collision instead.

For things made of several parts, turn on body on each part object, give each one a collider, and connect them with an empty set to fixed, hinge or ragdoll under joint (body a / body b). A hinge turns around the empty's x axis, so point that along the hinge.

Ragdolls: parent a collider box to each bone that should flop (bone parenting), turn on body (simulated) on the boxes and join neighbours with ragdoll joints. The body then drives its bone, and the skinned mesh follows. As is, it goes limp the moment it spawns. Type an event name into "Ragdoll event" to keep it animating instead until your mod calls `Unit.animation_event(unit, "that name")` (after `Unit.enable_animation_state_machine(unit)`), the same way the game's own enemies switch to ragdoll.

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

Walk and run cycles made for other games often move the whole character forward through the clip, so a looping state slides ahead and snaps back. Tick "In place" and the compiler takes that travel out of every clip (up and down movement stays) and writes each clip's speed into `build.log`, e.g. `clip 'walk' plays in place: ... move the unit at 0.78 m/s`. Move the unit from your mod at that speed while the clip plays and the feet stay planted.

For more than one animation, add states (one clip each) and transitions in the animation box. A transition switches to its target state when your mod sends its event:

```lua
Unit.enable_animation_state_machine(unit)
Unit.animation_event(unit, "walk")
```

## Rigging to a darktide skeleton

Anything that should move with a character (gloves, a helmet, a whole new body) has to use that character's skeleton. The addon reads the skeleton from the game files, so you need those extracted once. That's done with limn (https://github.com/manshanko/limn):

1. Download limn.exe from its releases page and put it in its own folder.
2. Copy `oo2core_9_win64.dll` from your game's `binaries` folder next to limn.exe.
3. Open command prompt and go to the limn folder (`cd /d "D:\path\to\limn folder"`).
4. Run this, with your own game folder after `-i` and any empty folder after `-o`:

   ```
   limn.exe --dump-raw -i "D:\SteamLibrary\steamapps\common\Warhammer 40,000 DARKTIDE\bundle" -o "D:\darktide_bones" unit bones
   ```

   Keep the quotes again. The `unit bones` at the end makes it only extract units and skeletons, otherwise you get the entire game. It takes a while and you end up with a folder full of `<hash>.unit` / `<hash>.bones` files. You only have to redo it if a game patch changes the skeletons.

In the asset collection's reference skeleton box set the game extract folder to that output folder (`D:\darktide_bones` above) and pick a preset: human, ogryn player, traitor guard (most renegade enemies) or flamer. The preset fills in the unit and bones files by itself, if it says they're missing the folder is wrong or the extract isn't done. Custom lets you point at any other unit/bones pair.

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

Anything you link to a first person unit (the arms or a held item) needs the weapon field of view, or it slides around against the hands when the view moves. The game sets that when the item is equipped, so do it yourself after linking:

```lua
Unit.set_shader_pass_flag_for_meshes_in_unit_and_childs(unit, "custom_fov", true)
```

## Replacing the game's own units

Everything a character wears or holds (gear, weapons, trinkets) is an item, and the game spawns the item's `base_unit`. Point that at your unit from your mod and the game uses yours, attached the same way as the original. Props, characters and the rest work the same if you swap them wherever your mod spawns them.

The game finds the things it hangs on a unit by node name, so a replacement needs the original's nodes, named the same and in the same places:

- `ap_` nodes are where other things attach. Weapons are a tree of part items (receiver, barrel, muzzle, magazine, sight, stock...) and each part links to an `ap_` node on the part before it, like the barrel on the receiver's `ap_barrel_01`.
- `fx_` nodes are where effects and sounds come out (muzzle flash from `fx_muzzle_01`, shells from `fx_eject`).
- Nodes named like the character's animated bones follow them. On weapons that's how the bolt, charging handle and mag release move (`ap_anim_01` to `ap_anim_10`, `ap_recharge_01`, `ap_release_01`, `ap_trigger_01` and so on), on gear it's the skeleton (see rigging above).

The addon copies the nodes for you:

1. Extract the game files with limn like in the rigging section above (the `unit bones` extract is enough) and set the game extract folder.
2. Type the resource path of the unit you're replacing into "Game unit" and click "Import Game Unit Nodes". You get an empty for every node of that unit, placed and named like the game's.
3. Some nodes only have a hash for a name and show up as `#1234abcd`. Leave those names alone, the compiler writes them back as that same hash.
4. Model around the empties and parent your meshes to them. Parent each moving piece to the node that moves it (your bolt to the `ap_anim_` node the game's bolt sits on). Don't rename, move or delete the empties, other parts and effects hang on them.
5. Compile like any other asset.

Then from your mod, with your custom assets package loaded (see the custom assets api), point the item at your unit and re-equip so it gets rebuilt:

```lua
local MasterItems = require("scripts/backend/master_items")
local item = MasterItems.get_cached()["content/items/..."] -- the item you're replacing
item.base_unit = "content/mods/my_mod/my_unit"
```

That changes it everywhere that item is used, and only for you. An item's `base_unit` is the unit path for step 2, and items made of parts list them under `attachments`, each with its own item name.

## Linux

On linux you use the same zip, the addon runs the exe through wine and converts the paths for you. You need wine and winepath installed. For textures point the compiler at your game's oodle dll once, using the same wine prefix you run blender with:

```
wine ./DarktideGLBCompiler.exe --configure-oodle "$(winepath -w "$HOME/.local/share/Steam/steamapps/common/Warhammer 40,000 DARKTIDE")"
```

I haven't tested this, I asked a llm, dont trust me.

## Known problems

- A ragdoll-ready character has no collision while it's still animating.
- Skinned assets with ragdoll bodies disappear when you get really close in first person.
- Shape keys get baked. The game has no shape keys at all, so that won't change.
- Only one uv map really gets used for the textures in a material.
- No cloth simulation (the game has none). Dangle bones are the substitute.
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
