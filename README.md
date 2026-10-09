# Darktide asset compiler (beta)

Blender addon + compiler for making your own models, props and gear for darktide. You do the work in blender, hit compile, and you get native darktide resources (unit, materials, textures, bones, animations, physics) plus a `build.json`.

Turning that output into something the game loads is done by the custom assets patcher, which is its own thing:
https://github.com/fviuff/darktide-mods/tree/main/Custom-Assets-patcher

This is a beta. Static props, materials, collision, skinned stuff that follows a character and simple animations work in game. Some things don't yet, see known problems at the bottom.

## Setup

Grab the release zip (same one for windows and linux). It has `DarktideGLBCompiler.exe`, `darktide_assets_blender.zip` and this readme.

In blender (made and tested on 5.2) go to edit > preferences > add-ons > install from disk, pick `darktide_assets_blender.zip` and enable it. If you had an older version installed, remove it first and restart blender. Press N in the 3d view and open the darktide tab. The panels go top to bottom in the order you use them: setup, asset, animation, flow, skeleton, game unit, active object, collision and physics, active material.

Fill in the setup panel once, it's remembered for every .blend (it's the addon's preferences, so you can also set it in edit > preferences > add-ons). Every path box has a little folder button at the end, click it to browse instead of typing:

- Compiler: `DarktideGLBCompiler.exe`.
- Darktide folder: your game folder, the one that has `binaries` and `bundle` in it. The normal steam location (`C:\Program Files (x86)\Steam\steamapps\common\Warhammer 40,000 DARKTIDE`) is filled in by itself. Textures get compressed with the game's oodle dll from in there, and game shaders read the game's files from it.
- Game files extract: a folder of the game's files pulled out with limn, see below. Skeletons, importing the game's particle effects, everything in the game unit panel and looking at the game's textures need it. A plain prop doesn't.
- Output path: where compiled assets go. That one belongs to the .blend, so save your .blend somewhere first.

### Extracting the game files

That's done with limn (https://github.com/manshanko/limn):

1. Download limn.exe from its releases page and put it in its own folder.
2. Copy `oo2core_9_win64.dll` from your game's `binaries` folder next to limn.exe.
3. Open the start menu, type `cmd` and open command prompt. Go to the limn folder. The `/d` is needed when it's on another drive than C:

   ```
   cd /d "D:\path\to\limn folder"
   ```

4. Run this, with your own game folder after `-i` and any empty folder after `-o`:

   ```
   limn.exe --dump-raw -i "D:\SteamLibrary\steamapps\common\Warhammer 40,000 DARKTIDE\bundle" -o "D:\darktide_extract" unit bones particles material texture state_machine
   ```

   Keep the quotes around the paths, they have spaces in them and without quotes it doesn't work. The `unit bones particles material texture state_machine` at the end makes it only extract what the addon reads, otherwise you get the entire game. It takes a while and you end up with a folder full of `<hash>.unit`, `<hash>.bones` and so on.

Set game files extract in the setup panel to that output folder (`D:\darktide_extract` above). Redo it after a game patch if something you use changed.

Running the compiler yourself from cmd instead of from blender? Then tell it your game folder once, unless the game is in the normal steam location: `DarktideGLBCompiler.exe --configure-oodle "D:\SteamLibrary\steamapps\common\Warhammer 40,000 DARKTIDE"`. If it worked it says `Oodle configured.` From blender the setup panel takes care of it.

## Your first prop

1. Make something simple, the default cube is fine. Apply scale (ctrl+a > scale).
2. Give it a material. A normal principled bsdf with a color and roughness is enough.
3. Select it in the 3d view and click "New Asset from Selection" in the asset panel. That makes an asset collection, one collection per asset.
4. Set the collection's resource path to something like `content/mods/my_mod/props/test_crate`. Lowercase only, keep it the same every time you rebuild, and use a different one for every asset.
5. Click "Inspect Asset" if you want a quick check of what will be exported, then "Compile Asset".

About selecting: select the objects themselves in the 3d view (drag a box around the whole model, or A for everything). Clicking a collection in the outliner selects nothing, and you end up with an empty asset. For a rigged model select the armature and all its meshes. Children of what you select come along by themselves, and so does the armature a mesh is skinned to. If you already made the asset and forgot something, select everything again and use "Replace Asset Members from Selection".

You get a folder named after the asset in your output path with the unit, materials, textures, `build.json` and `build.log`. If something fails, `build.log` is the first place to look. Inspect and compile also leave a `.glb` in the output path, which is exactly what the addon handed to the compiler. Import it into an empty blender scene if something looks off in game, that's what the compiler saw.

To get it in game, drop that folder into `mods/<YourMod>/Custom/<folder>/`, start the game (the patcher puts it in when the game starts), and spawn or use it from your mod through the custom assets api.

## Materials

The compiler reads the normal gltf style pbr setup: base color, metallic, roughness, alpha, normal map and emission, with or without image textures. Hook normal maps up through a normal map node, color images as srgb, the rest as non-color. Random shader node setups don't translate, keep it simple. Images go in through normal image texture nodes, so you pick them with blender's own open button there.

A single emission shader plugged straight into the output makes a cheap self lit material with no textures needed.

Faces only show from the front in game. For leaves, cloth, flat signs and other thin stuff tick "Double-sided" on the material, then the back gets added as extra triangles. Blender's own backface culling setting doesn't matter for this.

Making a weapon or gear? Tick "Weapon or gear materials" in the asset panel. Your materials then use the game's weapon shaders, so level stuff like snow and dirt decals doesn't land on them, same as the game's own weapons. Leave it off for props that stand around in a level. (Alpha clip and see-through materials stay as they are.) Glow is toned down to the game's weapon level: emission strength 1 is a normal weapon glow, go higher for a brighter one (too high and it turns white).

If you want to use a material that already exists in the game (or in your mod), set the material intent (active material panel) to use external material and type its resource path.

For glass, water, holograms, glowing or pulsing surfaces, crystal, fur and the like, set the intent to Game shader and pick one. It copies one of the game's own materials using that shader, and the panel lists everything that shader has, like the property editor in stingray:

- Settings: tick the box in front of one to change it, then set the color or numbers. Unticked ones keep the game's value.
- Textures: every texture slot of the shader with a dropdown. "Game's texture" keeps what the game has in there, or pick one of the images of your own material (the one plugged into base color, the normal map, the orm or emission). So put your images into the normal principled setup first, then pick them here.

Some settings only have a hash for a name, like `#1234abcd`, they work the same. The shader itself is copied into your asset too, so it works anywhere, not just where the game happens to have it loaded. Unlit color draws its color as raw brightness: around 0.1 per channel is a full color, 1 and up blows out to white.

"Weapon with coatings" is the shader the game's own weapons use for weapon skins: `base_bc`, `base_nm` and `base_orm` are your color, normal and orm images and `coat_mask` says where the skin goes (white = skin). "Weapon" is the same without skins.

Texture transforms: offsets, 90 degree turns and flips are fine, fractional scaling and odd rotations are not.

## Lights

Point and spot lamps in the collection become lights on the unit and move with whatever they're parented to. Brightness is blender watts times 60, so a freshly added 10 W lamp is a normal game lamp (600). Color comes straight from the lamp and the range is 8 m unless the light has a custom distance. Lamps cast shadows when their Shadow box is ticked (blender's default), untick it for cheap fill lights. Sun lamps are skipped. `Unit.light(unit, index)` plus the `Light.*` functions change them at runtime.

## Unit data

Units can carry their own values your scripts read with `Unit.get_data(unit, "name")`, like the game's pickups do (`pickup_type` and so on). Select the asset collection in the outliner, open the collection properties tab, and add them under custom properties. Text, numbers, true/false all work. For groups and lists set the property type to python and type the value like `{"speed": 2.5}` or `[1, 2, 3]`. A group gives you nested values (`Unit.get_data(unit, "group", "name")`) and a list gives you values by number, starting at 1 (`Unit.get_data(unit, "list", 1)`).

## Visibility groups

Type a name into "Visibility group" on an object (active object panel) and its meshes, plus every mesh parented under it, can be hidden and shown together: `Unit.set_visibility(unit, "name", false)`. Handy for variants, a helmet visor, a lid, that kind of thing. An empty works fine as the group holder.

## Flow (things that happen by themselves or on an event)

Flow is the little node graph a unit carries, same idea as the flow editor in stingray. It's how game units start their effects when they spawn or hide a part when a script tells them to.

1. In the flow panel click the + next to "Flow". That makes a flow for this asset with a "Unit Spawned" node already in it.
2. Open a node editor (any editor area, switch its type to node editor) and pick "Darktide Flow" as the tree type at the top, then your flow.
3. Shift+a adds nodes. Yellow sockets are events (when something happens), the rest are values. Connect events left to right.

The nodes:

- Unit Spawned: fires once when the unit spawns.
- Unit Unspawned: fires when the unit gets destroyed, e.g. to leave an effect behind.
- Flow Event: fires when your mod calls `Unit.flow_event(unit, "name")`. Type the name on the node.
- Particle Effect: create plays it, stop stops spawning new particles, kill removes it. "effect" is the resource name of a particle effect on an empty in this same asset (just the name you gave it), or a full path. "object" is the node it sits on, e.g. the name of an empty.
- Set Visibility: shows or hides a visibility group (empty group = the whole unit).
- Animation Event: sends an event to the unit's state machine.
- Delay, Once, Gate: wait, let something through only once, or let events through while open.
- Branch: fires true or false depending on its condition.
- Compare: compares a with b and fires every output that holds (less, less or equal, equal, greater or equal, greater).
- Fork: fires out 1 to out 8 one after the other.
- Counter: holds a number. It starts at start, add and subtract change it by step, reset puts it back. Its value output can go into a Compare, e.g. to do something on the third hit.
- Random Number, Vector3: a random number between min and max, or a vector3 from x, y, z.
- Get Mesh, Get Material, Set Material Variable: change a material while the unit is out there. Get Mesh takes the object's name, Get Material the material's name (both as named in blender; an object with several materials is one mesh per material, the name gives the first, `name_p1` the second and so on), Set Material Variable the variable. Glowing materials with textures or a base color have `intensity` (brightness) and `emissive_color`, a plain emission shader material has `emissive_intensity_lumen` and `emissive_color`, and a game shader material has the names its settings list shows. Pick Number or Color / vector on it for the kind of value.
- Get Unit Data (bool / number): reads a value from the unit's data (see Unit data), e.g. into a Branch.
- Get Light, Set Light Intensity, Set Light Color: change a lamp of the unit while it's out there, e.g. a flickering or alarm light. Get Light takes the lamp's name in blender. Intensity is in the same units as the export (blender watts times 60), color is red, green, blue from 0 to 1.
- Call Lua: runs a lua function when its event comes in. Type the function name, then list what it gets under Inputs and what it gives back under Outputs as `name:kind` with commas, e.g. `unit:unit, amount:float` (kinds: unit, bool, uint, float, vector3, quaternion, string, name). Every name becomes a socket. Under Events list the output events, `out` by default.

A unit input you leave unconnected means the unit itself.

If a unit's flow starts effects, release its custom assets package a frame after you destroy the unit, not in the same frame. The effect goes away on the next world update and still uses the package until then (closing the game unloads released packages right away and crashes on that).

The function lives in `FlowCallbacks`, the same table the game's own flow functions are in, so your mod adds it like this:

```lua
FlowCallbacks = FlowCallbacks or {}
FlowCallbacks.my_function = function(params)
    -- params.unit, params.amount ... (plus params.node_id)
    return { doubled = params.amount * 2, big = params.amount > 2 }
end
```

The table you return fills the outputs by name. An event fires when you set it to true in there, `out` fires unless you set it to false. The game's own functions work too, e.g. `set_unit_material_scalar` with inputs `unit:unit, material_name:string, variable_name:string, scalar:float` changes a value of one of the unit's materials.

## Shadows and shadow-only meshes

Untick "Casts shadow" on an object and its meshes (plus everything parented under it) draw without a shadow, good for glowy bits and small details. Untick "Visible" instead and it only shows up in shadows, so you can give a detailed model a cheap simple shadow: put a low poly copy next to it with Visible off, and turn Casts shadow off on the detailed one.

## LODs (simpler versions far away)

Make your model a few times with less and less detail. On each version type the same name into "LOD group" (use `lod`, that's what the game's units use), set "Level" (0 = full detail, 1 = simpler, and so on) and "Visible down to": how much of the screen height the thing has to fill before the next level takes over (the game uses stuff like 1.8 / 1.1 / 0.2, or 0.5 / 0.33 / 0.25). Give the last level 0 so it never disappears. Everything parented under a version belongs to that version. Without LODs the full model just always draws, which is fine for most things.

## Dangling bits

Straps, cables, pouches, tassels: select a bone, open bone properties and tick "Darktide Dangle". It then swings under gravity in game like the dangly bits on the game's own gear. Skin the swinging mesh to the bone as usual. A chain of dangle bones makes a floppy strap. Length (0 = the bone's length), damping, stiffness (pull back to rest) and max angle tune it.

Switch the mode to Jiggle for things that should bounce in place instead of swing (pouches, bellies, antennae): the bone's position springs behind where the animation puts it. Stiffness, damping and max stretch tune it.

It runs inside the unit's animation state machine, so your mod needs `Unit.enable_animation_state_machine(unit)` after spawning (units without animations get a still state made for them). There is no real cloth simulation in the game; this is what it uses instead.

## Aiming bones

A head that follows the player, a turret that tracks something, a gun arm pointing at a target: select the bone at the end of what should point (the head, or a small bone at the muzzle), open bone properties and tick "Darktide Aim". Give the target a name and list under Turn the bones that turn toward it, with how much each one takes, e.g. on a darktide skeleton with `j_head` ticked: `j_spine2:0.2, j_neck:1` (leave it empty and the bone's parent turns). The ticked bone itself can't be in the list, it's the end that points. Your mod moves the target every frame, in world positions, the same way the game aims its enemies:

```lua
local target = Unit.animation_find_constraint_target(unit, "aim_target")
Unit.animation_set_constraint_target(unit, target, position_to_look_at)
```

It runs in the state machine like dangling bits, so `Unit.enable_animation_state_machine(unit)` after spawning.

## Particle effects

Particle effects live on an empty. You can build one from scratch or start from any of the game's own effects (fire, sparks, smoke, steam, muzzle flashes...) and change whatever you like. The materials, shaders and textures an effect draws with come along as copies, so it works anywhere your asset is loaded.

1. Only needed for importing the game's effects and looking at their textures: set game files extract in setup.
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
5. Click "Preview" to watch it in the 3d view. It runs the effect the way the game does and restarts by itself when a short effect is done, every edit restarts it too. The sprites are drawn with their material's own shader from the game, so you see the real texture, flipbook, color ramp and glow (that needs the game files extract, without it they show as soft dots). What the game takes from the level isn't there: no fog, no shadows, no fading into walls, and lit smoke gets a plain white light. Mesh, light, ribbon and gpu visualizers aren't drawn, the panel lists anything the preview skips.
6. Click "Load" under materials to see every material the effect draws with, its current values and its texture channels. Click a texture channel to open that game texture as an image. Change what you want: "Values" like `#423d7a88=0.04,1,0,1` (names the addon can't read show as `#1234abcd`, they work all the same), "Textures" like `diffuse_map=//my_spark.png` (`//` means next to your .blend).
7. Compile. The effect gets the resource name you see on the empty, in your asset's folder.

Heads up: a billboard's vertex channels have to match what its material's shader reads, so when you swap the material, take the vertex channels and writers from a game effect that uses that material (import it on a spare empty and copy them over).

From your mod, once the asset is loaded:

```lua
local id = World.create_particles(world, "content/mods/my_mod/green_fire", position)
World.link_particles(world, id, unit, node_index, Matrix4x4.identity(), "stop") -- optional, follow a unit
World.destroy_particles(world, id)
```

Effects that draw game models as particles (debris, shell casings and the like) bring copies of those models along. A few whose models need more than materials (bones, animation) can't be copied yet.

## Collision and physics

This is all in the collision and physics panel. For a plain solid prop turn on use visible meshes as collision and leave body behavior on fixed collider. For cleaner collision, "Create Editable Collision Mesh" makes a separate collision-only copy you can simplify.

Body behavior options:
- fixed collider: doesn't move, players and everything else bump into it
- simulated body: real physics, falls and gets pushed around. Uses a convex hull, so concave stuff needs a few separate convex pieces
- moved externally: kinematic, your code moves it

Heads up: simulated bodies collide with the level and props but not with players. That's how the game's own loose props work too. If players need to bump into it (like a door), make it animated with fixed or moved externally collision instead.

Collision meshes can also be a capsule, sphere or box instead of the mesh itself: the shape gets fitted to the mesh along its own direction (a capsule runs along its longest side). Lighter than a hull, and what the game's ragdolls and hit boxes mostly use.

For things made of several parts, turn on body on each part object, give each one a collider, and connect them with an empty set to a joint type under joint (body a / body b):
- fixed: glued together (until it breaks, see below)
- hinge: turns around the empty's x axis, so point that along the hinge. Limit twist stops it at an angle, like a door against its frame
- slider: slides along the empty's x axis, like a drawer. Limit travel sets how far, in meters from where it sits in blender
- ball: turns freely in every direction
- ragdoll: swings and twists within the swing and twist limits, like a body joint

Spring pulls the joint back to how it sits in blender, so a door swings shut by itself or a drawer slides back in, and spring damping makes it settle instead of bouncing (a door of 6 kg: spring 20, damping 4). Break force and break torque make the joint snap when it gets pushed or twisted harder than that (in newtons, 0 = never), for things that should come apart.

Ragdolls: parent a collider box to each bone that should flop (bone parenting), turn on body (simulated) on the boxes and join neighbours with ragdoll joints. The body then drives its bone, and the skinned mesh follows. As is, it goes limp the moment it spawns. Type an event name into "Ragdoll event" to keep it animating instead until your mod calls `Unit.animation_event(unit, "that name")` (after `Unit.enable_animation_state_machine(unit)`), the same way the game's own enemies switch to ragdoll. Use the ragdoll event way if your rig has a bone above the hips without a body (like a root bone), see known problems.

A spawned simulated prop can sit there asleep until something touches it. `Actor.wake_up(actor)` sorts that out.

"Own actor" (active object panel, meshes only) makes a mesh a collision shape of its own that your scripts can find by name with `Unit.actor(unit, "name")`. The mesh isn't drawn, it only gives the shape. "Hit zone (EXPERIMENTAL)" is the kind enemies have, see making an enemy. Untick "Created at spawn" for one your script creates later with `Unit.create_actor`.

"Mover" (collision panel) is the upright capsule the game walks characters through the level with, the one `Unit.set_mover` and the `Mover.*` functions use. Enemies use `filter_minion_mover`, player characters `filter_player_mover`.

## Animation

Make an action per animation. If you want more than one, push each finished action down to its own nla track (dope sheet > action editor > push down) and name them something useful, you pick them by that name later. A plain animated object works too, you don't need to rig a spinning fan.

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

Characters and enemies do it the other way: the game reads where the animation wants to go from `root_point`, the top bone of every darktide skeleton, and moves the character itself. Tick "Root motion" instead of in place and the compiler moves the travel from the hips onto `root_point` (`build.log` says how far). If you animate `root_point` yourself, leave both off.

### The state machine, step by step

Everything with more than one animation goes through a state machine. Some words first:

- a state is "what plays right now", usually one clip, looping or once
- an event is a word that gets sent to the unit, like `walk` or `attack`. Your mod sends them with `Unit.animation_event(unit, "walk")`, for enemies the game's own scripts send them
- a transition is "when event X comes in while in state A, go to state B and blend over N seconds"
- a variable is a number your mod can set, like speed, used by blends (below)

Making one, with an idle, a walk and an attack:

1. Make the three actions (idle, walk, attack) and push them down to nla tracks like above.
2. Click "Inspect Asset" once, so the clip dropdowns know your action names.
3. In the animation panel tick "Create event-driven state graph".
4. Click "Add State" three times. Name them idle, walk and attack and pick each one's clip in the dropdown. Idle and walk loop, attack plays once. State 0 (the top one) is where it starts.
5. Click "Add Transition" for each switch you want: from idle to walk on `walk`, from walk to idle on `idle`, and so on. Blend is how many seconds it takes to fade over, 0.2 is a good start.
6. Attack only plays once, so it has to get back to idle by itself: give the attack state the exit event `done` and add a transition from attack to idle on `done`. Set its "Seconds left" to the same as that transition's blend, then idle is fully in when the attack ends.
7. Click "Play State Machine" under the states. It plays your graph on your rig the way the game runs it, and you get a button for every event, click `walk` and see it go over. Click "Play State Machine" again to stop, your rig gets its pose back.
8. Compile. From your mod:

   ```lua
   Unit.enable_animation_state_machine(unit)
   Unit.animation_event(unit, "walk")
   ```

Shortcuts so you don't need a hundred rows:

- the globe button on a transition makes it work from every state. One row "from anywhere to idle on `idle`" covers it
- a `*` in the event name stands for anything. `attack_*` reacts to `attack_01`, `attack_02` and so on. A normal row for one state and event wins over these

### More state machine stuff

To mix clips smoothly (walk into run by speed) tick "Blend" on a state instead of picking one clip. Add a float variable (e.g. `speed` from 1 to 2), pick it on the state and list the clips as `clip:value` by name, e.g. `walk:1, run:2`. A clip plays fully when the variable sits on its value and fades into its neighbours in between. Change it from your mod:

```lua
Unit.animation_set_variable(unit, Unit.animation_find_variable(unit, "speed"), 1.5)
```

Tick 2D for a blend by two variables at once, like strafing (x sideways, y forward): pick both variables and list the clips as `clip:x:y`, e.g. `idle:0:0, right:1:0, left:-1:0, forward:0:1`. Put the clips on a grid (the same few x values and y values), each clip then fades into the ones next to it in both directions.

The clips in a blend play in step: they all get stretched to the length of the first one in the list. So blend clips of the same kind, like a walk cycle and a run cycle with the feet hitting the ground at the same points, and give standing around its own state with a transition. The mix follows the variable right away, so for a smooth change move the variable a bit every frame instead of jumping it.

A state can send events itself. "Events at" sends them when its clip reaches a time, as `seconds:event`, e.g. `0.4:hit` (the transitions react the same as to `Unit.animation_event`, in every layer). "Exit event" goes out once just before the clip ends, see step 6 above.

For variety tick "Random" on a state: it plays one of several clips picked at random. List them under Clips as `clip:weight`, e.g. `idle_a:3, idle_b:1` plays idle_a three times as often as idle_b. Pick says when it picks again: every loop, every loop but never the same clip twice in a row, or once when the state starts.

Every state has a speed, 1 plays it as animated, 2 twice as fast. Tick "Speed from variable" and pick a float variable instead to change it while it plays, like a walk that speeds up with the character, by setting that variable from your mod the same way.

Layers play states on top of each other, like waving while walking. Set a state's Layer to 1 (or higher) and it plays over layer 0 instead of replacing it. Each layer runs on its own: it starts in its first state, and its transitions stay inside it, but an event reaches every layer at once. Usually a layer starts in an Empty state (plays nothing, so the layers below show through) with a transition to the real one and back. Bones picks what a state moves: select the bones on your rig in pose mode and click the bone button next to Bones, a bone always takes everything below it along (on a darktide skeleton `j_spine1` is the whole upper body). Add `:0.5` after a name to move it only halfway, e.g. `j_neck:0.5`, and later entries win, so `j_spine1, j_leftshoulder:0` is the upper body without the left arm. Leave it empty for the whole body. Additive adds the clip on top of what plays below instead of replacing it, good for a breathing or flinch motion.

## Rigging to a darktide skeleton

Anything that should move with a character (gloves, a helmet, a whole new body) has to use that character's skeleton. The addon reads the skeleton from the game files, so set game files extract in setup first.

In the skeleton panel pick a preset. There's one for every character skeleton in the game: human and ogryn players (also their first person arms), traitor guard (most renegade enemies), flamer, traitor captain, rager, chaos ogryn, plague ogryn, poxwalker, poxburster, pox hound, mutant, daemonhost, beast of nurgle, chaos spawn, imperial humans, hadron, the companion dog and the servo skull. The preset fills in the unit and bones files by itself, if it says they're missing the extract folder is wrong or the extract isn't done. Custom lets you point at any other unit/bones pair.

Easiest way to start is "New Starter Asset from Skeleton". Pick a skeleton and hands, head or full body proxy and you get the real armature with a simple mesh already weighted to the right bones. Reshape it into whatever you want and compile. "Import Rest-Pose Reference Skeleton" gives you just the armature if you'd rather model from scratch.

### Using a model you already have

A model rigged somewhere else has different bone names, different proportions and usually a pile of extra bones (cloth, jiggle, holders and so on). The addon can convert it:

1. Import it, select the armature and meshes, "New Asset from Selection".
2. Pick the preset like above.
3. Click "Auto-map Bones". It pairs your bones with darktide bones from their names and the rig structure. Go through the list anyway. Hips, spine, neck, head, shoulders, arms, hands, fingers, legs and feet should all be paired. Fix wrong ones with the search field. Leave cloth/jiggle/weapon/helper bones empty, they get kept as extra bones that just follow their parent.
4. Click "Fit to Darktide Skeleton". It scales and poses the model onto the darktide rest pose, moves the meshes and shape keys with it, skins stuff that was just parented to a bone, and swaps in a new armature with the darktide names plus your extras. The old one gets hidden. Read the little report it gives you.
5. Pose a few bones to check it looks right, then compile. Only weighted bones (gear) gets switched on after a fit so only bones your meshes actually use end up in the build.

The model ends up with the darktide skeleton's proportions, since the game's animations drive those bones.

### MMD models

MMD models (`.pmx`) open straight in the addon, you don't need mmd tools: file > import > MMD Model (.pmx). Keep the textures where they came with the model (usually a folder next to the .pmx), they get loaded from there. Then it's the same steps as above: select the armature and the mesh, New Asset from Selection, pick human, Auto-map Bones, Fit to Darktide Skeleton, compile.

Auto-map reads the japanese bone names MMD uses (下半身, 上半身, 左腕, 左ひじ, 左足...) and the english ones from cats or mmd tools (Left arm, Left elbow, Thumb0_L...). The leg "D" bones some models have (左足D...) carry the leg weights, so those get paired instead of the plain leg bones. IK, twist and the other helper bones stay out, they just follow along. MMD physics (hair and skirt rigid bodies) isn't taken along, so hair and skirts are stiff. Morphs come in as shape keys and get baked like any shape key.

A model that got converted to a mesh without a skeleton (some online converters do that) can't be fitted, get the .pmx.

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

1. Set game files extract in setup.
2. Type the resource path of the unit you're replacing into "Game unit" and click "Import Game Unit Nodes" (game unit panel). You get an empty for every node of that unit, placed and named like the game's.
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

## Making an enemy (EXPERIMENTAL)

Everything enemy is experimental. Good luck lol.


An enemy is two things: the breed (how it behaves, its health, what it attacks with, lua) and its unit (the model, skeleton, collision and animations). You make the unit here and the breed runs it like its own. Also only works locally (psykhanium, solo play mods), duhh.

1. Rig your model to the skeleton of the enemy whose breed you'll use (skeleton panel, the poxwalker, traitor guard and so on), with a starter asset or "Fit to Darktide Skeleton" like any character. It has to be that skeleton, the breed's scripts look bones up by name.
2. In the game unit panel type the game enemy's unit into "Game unit", e.g. `content/characters/enemy/chaos_poxwalker/third_person/base`, and click "Import Game Collision (EXPERIMENTAL)". You get:
   - its hit zones: capsules and spheres named `c_head`, `c_spine1` and so on, on your bones. The breed finds them by these names so keep them, but resize them to fit your model, they're plain meshes
   - its ragdoll: a body per bone with the game's masses and joints, and "Ragdoll event" set to `ragdoll`, which is what the game sends when it dies
   - its mover
   - every other node of the game's unit your rig doesn't have (attach points and the like), the breed hangs its weapon on those
   - "Package named like the unit" gets ticked. Leave it on, the game loads an enemy's body as a package with the unit's own path when a mission loads, and crashes if there isn't one
3. Make your animations: idle, walk, run, attacks, getting staggered and so on. Tick "Root motion" (see animation).
4. In the animation panel click "Import Game Events (EXPERIMENTAL)". This one is easy to get confused by, so: the game's scripts send the enemy's state machine a lot of events (a poxwalker sends 386 different ones), and the game crashes when one comes in that the state machine doesn't list. Import Game Events only lists them all, it doesn't make anything play. Making them play is the next step.
5. Make your states (step by step above) and hook the game's events to them with globe transitions and `*`. The ones a poxwalker uses most:
   - `idle`, and `to_1h_weapon` when it spawns
   - walking: `walk_*` and `move_*` (`walk_fwd`, `move_start_fwd` and friends)
   - attacks: `attack_*`
   - getting hit: `stagger_*` and `hit_reaction_*`
   - dying: `death_*` (it ragdolls on `ragdoll` by itself)

   An attack plays once, so it needs an exit event back to idle (step 6 of the state machine). To check it, click "Play State Machine": with that many events you get a search box instead of buttons, type part of an event like `attack` and send it, and see if your state plays. The breed files (`scripts/settings/breed/breed_actions/` in the game's source) list exactly which event each action sends.
6. Pick one of the two below, then compile.

### Your own breed (EXPERIMENTAL)

Tick "Make a breed (EXPERIMENTAL)" in the game unit panel, give it a lowercase name like `my_walker` and pick what it behaves like under "Behaves like" (Import Game Collision already picked the one that matches the game unit). Then:

- Body: "Own body" (default) only hangs the game breed's weapons on your model. Its other body pieces, wounds, gibbing and dissolving are left off, those only work on the game's own body pieces. "Game body pieces too" keeps all of that on top of your model
- Health: one number is its health on the easiest difficulty, the harder ones scale like the game breed's (a poxwalker has 150, so 300 is double on every difficulty). Five numbers set every difficulty yourself. Empty keeps the game breed's
- Walk and run speed in meters per second, 0 keeps the game breed's
- Hit zone group on each hit zone (collision panel): what a hit there counts as. "From the game breed" uses what the game breed has for that name (c_head is head). Your own extra hit zones (a tail, a second head) need a group

Compiling then also writes `my_walker_breed.lua` next to the asset folder in your output path. Copy it into your mod's scripts folder, load it once and spawn it whenever your asset's package is loaded:

```lua
local breed_name = mod:io_dofile("MyMod/scripts/mods/MyMod/my_walker_breed")
Managers.state.minion_spawn:spawn_minion(breed_name, position, rotation, 2) -- 2 = the enemy side
```

### Taking over a game breed

Or put your unit on one of the game's breeds, then every one of those your game spawns is yours:

```lua
local Breeds = require("scripts/settings/breed/breeds")
Breeds.chaos_poxwalker.base_unit = "content/mods/my_mod/my_enemy"
```

That way the breed still hangs all its own body pieces and its weapon on your unit by node name (listed per enemy in `scripts/settings/minion_visual_loadout/templates/`), and its wounds and gibbing work on those pieces.

## Linux

On linux you use the same zip, the addon runs the exe through wine and converts the paths for you. You need wine and winepath installed. Set the darktide folder in setup to your linux game folder, the addon hands it to the compiler. Running the compiler yourself, point it at the game once, using the same wine prefix you run blender with:

```
wine ./DarktideGLBCompiler.exe --configure-oodle "$(winepath -w "$HOME/.local/share/Steam/steamapps/common/Warhammer 40,000 DARKTIDE")"
```

I haven't tested this, I asked a llm, dont trust me.

## Known problems

- A ragdoll that goes limp the moment it spawns (no ragdoll event) can disappear when you get really close, if its rig has a bone above the hips without a body. With a ragdoll event it doesn't.
- Weapon skins don't recolour "Weapon with coatings" parts yet.
- Shape keys get baked. The game has no shape keys at all, so that won't change.
- Only one uv map really gets used for the textures in a material.
- No cloth simulation (the game has none). Dangle bones are the substitute.
- Auto-map needs an actual head bone. On rigs without one, check the head row before fitting.
- Gltf extras like clearcoat, sheen and transmission are approximated with plain pbr.

## Common stuff

- Compiler not found: set the compiler in setup to the exe. On linux also check wine and winepath.
- A member is missing or excluded: the collection has to be enabled in the view layer and its objects visible and selectable.
- Something missing from the output: check the collection members, then compare the inspect report and `build.log`.
- Texture or oodle errors: check the darktide folder in setup.
- My animated thing just stands there: nothing plays automatically, call `Unit.play_simple_animation` or enable the state machine from your mod.

Keep your .blend and source images around, the build output doesn't include them. One output folder per resource path.

Third party licenses are in `licenses/` and `THIRD_PARTY_LICENSES.txt`.

## On the list

Stuff it can't quite do yet but that's being worked on:

- Your own shaders. Right now materials use the game's shaders (the generated ones, weapon materials and game shader picks). Writing a shader of your own, from code or nodes, isn't there yet.
- Player armour and clothing shaders, the ones with the cosmetic colour options.
- New particle shaders. Effects use the game's particle materials, with your own values and textures.
- More flow nodes: sub-flows, two-unit events and the remaining light settings (temperature, falloff) among others.
- More state machine bits: chain bones (tails and cables that swing as a chain), timeline triggers that call your lua, events a set time before a clip ends, beat sync, states that move the unit by their root motion themselves, and playing your clips on the game's own state machines.
- Occluders, culling settings and surface queries per mesh.
- Choosing texture compression and mip settings yourself. For now the right settings get picked per texture slot.
- A second uv map, and atlases that mix uv maps.
- Previews in blender for dangle and jiggle bones, aiming bones, game shader materials on meshes, joint limits and springs, and lod distances. Ragdoll and bone-constraint states don't show in the state machine preview yet.
- Particle preview: fog, shadows, the level's lighting, fading where particles meet walls, and mesh, light, ribbon and gpu visualizers.
