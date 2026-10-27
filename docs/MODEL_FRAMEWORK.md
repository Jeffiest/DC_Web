# Model framework (r3)

A plan for moving Dark Cloud's models, characters and animations to glTF 2.0, converted at
extraction and loaded natively, as ChronicleTwo does for Dark Cloud 2: models become ordinary glTF
files that tools such as Blender open, animations become named actions, and a datapack can replace
a model with its own glTF. Cloth keeps its own section (section 8), with an optional Enhanced mode
that keeps it out of the body.

What isn't particular to Dark Cloud goes into Kazarov, as for audio (docs/AUDIO_FRAMEWORK.md): the
converter and the motion name table. Chronicle keeps what only it has: its character configuration
commands, its glTF loader, its cloth and the hooks from its retail loaders. The glTF
loader is Chronicle's own for now (section 5); it can be unified with ChronicleTwo's once both games
are ready.

The four phases are built; section 12 says where each stands and what is left for later.
Sections 2 and 3 describe what existed and what was measured before.

---

## 1. Goals

- **Open models.** Every model on the disc, characters, weapons, items, scenes and georama parts
  alike, published as glTF 2.0, with its meshes, materials, textures, skeleton, skin weights and morphs, and its motions as
  named actions in English.
- **The game unchanged.** The converted data plays exactly as the retail files do: the same poses
  at the same frame numbers, the same attachment points, hit windows and events. Retail code keeps
  addressing motions by frame number, as it does today.
- **Moddable.** A datapack can replace any model with its own glTF, made in a standard tool.
- **Shared.** The converter and the general glTF loading live in Kazarov and serve both games.
- **Cloth that can be better.** Dark Cloud's cloth data converts as it is, and an optional mode keeps
  the cloth out of the arms and legs (section 8).

---

## 2. Today

**Formats.** Dark Cloud and Dark Cloud 2 share Level-5's engine formats:

| Format | Holds | Dark Cloud loader |
|---|---|---|
| MDS | a scene: one record per object (name, parent, local matrix, offset of its MDT) | `LoadMDSFile` (`dataset.cpp`) |
| MDT | a mesh inside an MDS: positions, normals, colours, UVs, strips with per-corner indices, materials | `CVisualVu1::CreateVUdataFromMDT` (`port/src/game/visualvu1.cpp`) |
| MOT | keyframe channels: rotation, scale, translation, vertex morphs, camera, material, visibility | `CreateAnimeDataEX` (`gameutil.cpp`) |
| WGT | skin weights in the same channel format: per mesh frame, bone and vertex, a percentage | `CreateAnimeDataEX` |
| BBP | bind-pose matrices, one per frame | `CreateAnimeDataEX` |
| CHR | a pack of the above with a character `.cfg` and textures | `CCharacter::LoadPackData*`, `ReadInfo` (`chararead.cpp`) |
| CLO | cloth parameters (section 8) | `InitCloth` (`clothread.cpp`) |

The record layouts match what Kazarov's converter parses field for field: MDS objects (name at 8,
MDT at 40, parent at 44, matrix at 48), the MDT header's count and offset pairs, 96-byte materials,
and the 32-byte MOT and WGT channel and key records with the same type numbers. One difference is
known: Dark Cloud's material texture name runs 44 bytes where Dark Cloud 2 reads 32 and then flags.

**The port today** uses the retail files in place. `LoadMDSFile` is not replaced; meshes are built
for Kazarov's renderer from the MDT (`visualvu1.cpp`), skinning runs on the CPU (`MotionProc2`,
`port/src/game/gameutil.cpp`), and the display list interpolates frame matrices and rewritten vertices
between ticks. Kazarov's GPU skinning path (prog0 palettes) is Dark Cloud 2's and unused here.

**What reads models.** About 92 calls of `LoadMDSFile` (half in the title and opening), about 113
of `LoadPackData*`, 14 of `LoadCollisionFile`, and 4 of `CMotionModel::LoadPack`. Game logic finds
frames by name (`SearchFrame`, about 150 calls: `weapon`, `r_foot`, `dcol0`...), attaches weapons
(`SetReference`), and compares a character's motion time with absolute frame numbers: a hit window
is `time >= 674 && time <= 677` (`charakey.cpp`). So frame numbers are part of the game's logic.

**Kazarov's converter** (`tools/kazarov/src/rust/models.rs`, 8,475 lines; `actions.rs`, 503)
publishes Dark Cloud 2's models as glTF: nodes from MDS objects, meshes with skin joints and weights,
inverse binds from BBP, morph targets from vertex channels, PBR materials with the converted
textures, animations sampled at 30 frames per second, and a `.game.json` sidecar holding only the
game semantics glTF can't express. Actions are named through `assets/motion_names.json` (2,870
entries). Its parsers carry no version checks; the Dark Cloud 2 specific parts are the player-unit
tables, external motion placement from DC2's scripts, DC2's disc layout and the motion table. The C++
bridge exposes no model function yet.

**ChronicleTwo's runtime** (`port/src/platform/model_assets.cpp`, about 3,700 lines) reads the glTF
with fastgltf and fills the retail structures directly (frames, mesh visuals, motion lists), through
about a dozen replaced loaders. Animations are evaluated natively from the glTF samplers, each action
given a range of game frames. Its general parts (document and datapack reading, accessors, skins,
sampling) are a few hundred lines written against Dark Cloud 2's structures.

---

## 3. Measured

On the PAL disc's extracted data:

| | Count | Size |
|---|---|---|
| Character packs (`.chr`) | 1,116 | 495 MB |
| Models (`.mds`), loose and in packs | 412 and 5,113 | 6 and 148 MB |
| Motions (`.mot`), in packs | 2,455 | 307 MB |
| Skin weights (`.wgt`) and bind poses (`.bbp`) | 1,360 and 1,424 | 36 and 4 MB |
| Cloth (`.clo`) | 104 in packs, 7 loose | small |
| Character `.cfg` files | 1,265, with 9,573 `KEY` lines | |

**Kazarov's converter, unchanged, on Dark Cloud's packs.** Its own `retail_pack_smoke` test converts
Toan's pack (`chara/c01d.chr`) and a monster's (`chara/f01a.chr`) and passes its validation: Toan's
462 nodes, 26 meshes and 210 primitives, the monster's skeleton and skin. Three things don't come
across yet, all from Dark Cloud's character `.cfg`:

- **Actions.** A Dark Cloud motion file is one timeline cut into actions by `KEY start, end, speed`
  lines, named only in a trailing comment: `KEY 10, 20, 0.05 //立ち 0` (standing). The converter
  doesn't read that syntax, so each file becomes one animation. 9,419 of the 9,573 keys carry a
  name, 2,391 distinct; Dark Cloud 2's table already translates 237 of them, covering 36% of keys.
- **Toan's skin.** `MOTION 0, "c01dbas.mot", "c01dbas.bbp", "c01dbas.wgt"` names the three files
  together; Toan converts without a skin, likely because this form isn't read.
- **Dark Cloud's own commands:** `TEX_ANIME` (Toan's blinking eyes), `MATERIAL_ANIME`,
  `VERTEX_ANIME`, `BODY_SIZE`, `ALLOC_MDT`, `ALLOC_DBUFF`, `FOOT`, `EVENT`, `CLOTH`.

---

## 4. Extraction

`dcdata` converts the models through Kazarov's converter, the one Dark Cloud 2 uses, with a **game
profile** saying how Dark Cloud's data differs:

- **Kazarov** (`models_convert_dc1`) converts the textures and then every model, motion, weight
  and bind pose into one tree, so each glTF points at the PNGs beside it. The profile carries:
  - **the game's configuration reader.** Chronicle hands Kazarov a reader (`ProfileReader`) that
    turns a `.cfg` or `.clo` into typed commands; Kazarov doesn't know Dark Cloud's syntax.
  - **absolute frames.** Actions and `KEY` commands keep their source frames.
  - **fixed strip streams.** Every Dark Cloud strip corner indexes a position, normal and texture
    coordinate, plus a colour when the mesh has colours, as `CVisualVu1::CreateVUdataFromMDT`
    reads them; Dark Cloud 2's prim-word flags don't apply.
  - **source records.** Sidecars keep what rebuilding each source file byte for byte needs (its
    header and reserved words, local matrices, whole material records, the words no reader looks
    at), and `data/models.json` indexes every source by its retail name.
- **Chronicle** (`tools/dcdata/character_profile.hpp`) reads `.cfg` and `.clo` word for word as
  `ReadInfo`, `CTextureAnime::LoadCFGFile` and `InitCloth` do. A `KEY`'s name is the comment on its
  line, without the index after it (`KEY 10, 20, 0.05 //立ち 0`), and becomes Dark Cloud 2's
  form, `KEY "立ち", 10, 20, 0.05`. A comment that is only the index leaves the key unnamed; it
  takes its motion's name.

Dark Cloud's data needed a few things the converter now handles for any game:

- **Collision meshes.** An MDT counting no texture coordinates holds collision triangles
  (`MDT_COLLISION`: vertex indices, an attribute index and a reserved word), as every such MDT on
  the disc does. It becomes a triangle mesh, its attribute records kept in the sidecar.
- **Indices past a stream's count.** Some strips and skins index a vertex past its stream's count
  that the mesh still holds; the converter reads it from the mesh's own bytes, as the game does.
- **Unused key lanes** whose bits aren't numbers keep their words.
- **Animated memory card icons**: the other shapes become morph targets.

| Disc | Published |
|---|---|
| a character pack, `chara/c01d.chr` | `chara/c01d.gltf` (+ `.gltf.bin`) and `c01d.game.json`, the sidecar, which also holds its configuration as typed commands |
| a loose model, `dun/item/main_data/<name>.mds` | `<name>.gltf` and a minimal `.game.json` |
| a cloth file | its typed commands in the character's sidecar (section 8) |
| textures a model uses | the PNGs the texture bake already writes, referenced from the glTF |

**Actions keep their frames.** Each `KEY` becomes one named glTF action, and the sidecar records its
original start and end frame. The runtime places each action back at those frames, so retail code
that asks for frame 674 still gets what the motion file holds at 674. Unlike ChronicleTwo, which
gives actions fresh frame ranges, Dark Cloud's absolute hit windows and event frames depend on this.

**Names.** An action's English name comes from Kazarov's shared table by its Japanese comment name
(`立ち` → `idle`). Dark Cloud adds 2,293 names to that table (5,163 now), translated for what the
motion is for in the game rather than word for word (`釣り上げられ中` is `being_reeled_in`). A
motion-slot number before a name becomes a suffix (`0立ち` → `idle_0`). An unnamed key takes its
motion's name (`c01dbas`, `c01dbas_v2`...).

**Checks** run on the real disc (section 10): every model, weight and bind pose rebuilt from its
bundle equals the disc's bytes, and every character motion keeps its poses in every action.

**Measured** on the PAL disc: all 5,166 files holding models convert, in about four and a half
minutes; 11,782 sources are indexed (6,737 models, 1,953 character motions, 1,409 bind poses, 1,342
weights). Kazarov places a motion pack's motions into every model they fit, as for Dark Cloud 2;
322 fit none and keep their retail keys.

---

## 5. Loading

The game's own loaders (`LoadMDSFile`, `LoadCollisionFile`, `CreateAnimeDataEX`, cloth, vertex
animation) stay as they are, and so do their 200-odd callers. What changes is where their records
come from: `port/src/data/model_gltf.cpp` rebuilds them from the published bundles.

- **When the game reads a file**, every published model, motion, weight and bind pose inside it,
  in nested packs and in scenes at offsets alike, is rebuilt from its bundle and written into the
  game's buffer where the disc has it. The game then edits it in place as it does on the PS2 (shadow
  edge flags, for one), and the bytes live as long as the buffer does.
- **Models, weights and bind poses** come back byte for byte: the glTF holds the geometry and the
  sidecar the rest.
- **Motions** come back from their glTF actions: each action's clip gives the transform keys inside
  it at their source frames. The sidecar keeps what a clip can't give back: the keys outside every
  action and on each action's first and last frame, the channels glTF has no place for (morphs,
  camera, material, visibility), and the rotation and scale keys of frames the converter rebaked
  from the pose. Every motion keeps the disc's key frames, so none grows past the disc's, whose
  sizes the game's arenas are made for.
- Frames keep their names, attributes and order, so `SearchFrame`, attachments and hit windows work
  as before.

Georama scenes (`.scn`), dungeon map parts (`.mpd`), the opening's scenes (`.sne`) and the title load
the same way: the converter finds their scenes, and the rebuild puts each back at its offset.

**Chronicle's own loader.** The glTF is read with fastgltf, which Kazarov already builds, by a loader
in Chronicle's `port/src`. ChronicleTwo's loader does the same
for Dark Cloud 2's; the general parts of the two (documents and their datapack candidates,
accessors, skins, the animation samplers) can move into Kazarov as one layer later, once both games
are ready for it.

**Skinning** stays on the CPU, as today. Kazarov's GPU skinning path can come later, measured.

---

## 6. Datapacks

**Later.** For now the loader takes a bundle only where it rebuilds the disc's bytes (section 12,
"Safeguards"), so a datapack's model isn't loaded yet. The plan:

A datapack replaces a model by its published path with its own glTF (and optionally its sidecar).
A replacement may change topology, materials and textures; its actions are matched by name, and a
replacement that lacks one of the original's actions, or is malformed, falls back to the published
model, as ChronicleTwo's loader does.

---

## 7. What lives where

| Kazarov (shared) | Chronicle |
|---|---|
| MDS/MDT/MOT/WGT/BBP/CHR to glTF; textures linked | the pack layout and which files are models |
| the character configuration as typed commands, from a game's table | Dark Cloud's command table and `.cfg` reading |
| the motion name table | Dark Cloud's names added to it |
| | the glTF loader, which rebuilds the retail records from the bundles |
| | `CCloth`'s step, original and enhanced (section 8), and the Options row |

---

## 8. Cloth

**What it is.** A `.clo` file sets up one piece of cloth: `SIZE` (a grid of up to 16 by 16 points),
`FRAME` (the object it hangs from), `GRAVITY`, `WINDEFFECT`, `FOLLOW` (how much of the body's
movement it copies, per axis), `K` (the stiffness of the springs pulling each point to its rest
shape), `BOUND` (a box on a named object it can't pass, with friction), `NORMAL` and `POLYDIV`. The
grid itself is a mesh of the model. `CCloth` steps it once per tick: each point springs towards its
rest place on its frame, keeps its rest distances to its neighbours, falls under gravity, takes the
wind, and is pushed out of the boxes and off the floor. Toan's turban and poncho are three of them.

**Converted.** The grid travels in the glTF as a mesh. Its parameters are published as typed
commands in the character's sidecar, as Dark Cloud 2's cloth commands are; the game still reads the
`.clo` itself, as the PS2 does.

**Original.** The PS2's simulation, as the port runs it now: the cloth looks and moves exactly as it
does on the PS2.

**Enhanced,** the PS2's step with the body added. After the step pushes the cloth out of the authored
boxes, it pushes it out of a capsule along each joint of the arms and legs (a chain of two joints,
`jntN_1` to `jntN_2` to `effN`), a fifth of the joint's length thick, the same way, so the push carries
into the speeds as a box's does. Two rules keep it from doing harm:

- A vertex comes no nearer a limb than it rested when the cloth settled on the body (first step, or
  after a cut): cloth authored lying against an arm isn't pushed off it, and an arm raised through
  where the cloth rests still pushes it. Skipping a limb wherever the rest place lay inside it let
  Toan's arms pass through the poncho's shoulders when he raises them over the snake in the attract
  movie: overlaps up to 0.74 deep, now at most 0.34 there.
- A limb pressing a vertex towards the torso (a one-joint chain whose end the other chains branch from:
  the pelvis, the spine up to the shoulders) stops it at the torso's surface, or where it rests if that
  is nearer. An arm swinging in front of Toan's poncho pressed it into his chest,
  which the PS2's cloth never shows; squeezed between the two, the cloth now stays on the chest and the
  arm overlaps it. The push is `cloth_body::PushOutOfBody` (`port/include/game/cloth.hpp`), tested on its own.

The torso's surface is the body's own mesh. When the cloth settles, the skinned meshes the character
draws (Toan's `skin`; a cloth's frame is not drawn, so the poncho's starting mesh is left out) are read
as they stand, each vertex given to the bone whose capsule surface it lies nearest (the head, hands and
feet among them, so a hand at his side doesn't widen him), and the torso's faces are measured from the
spine outwards in 12 directions at 6 heights, in the spine joint's frame so the shape turns with him.
The steps then only look the shape up. A round torso 0.4 of the spine's length thick, the first
version, reached 1.29 all round: right for Toan's chest (1.26 at its deepest) but 0.3 to 0.7 out behind
his back (0.6 at the waist, 1.0 at the shoulders), so it held the poncho's back panel off him (a
quarter of the time, by up to 0.4) where the PS2 lets it lie. Fitted, the back hangs as the PS2's does
(its rows' mean distances from the spine within 0.01) and never goes into his back, and the front
stays off the chest as well as before. A torso the mesh says too little about (fewer than 8 vertices,
or faces in under half the directions: Toan's pelvis, whose vertices the legs take) stays round.

The torso also keeps the cloth off the chest: out to its surface, or for cloth authored resting inside
it (Toan's collar, up to 0.8 deep in his chest mesh) no deeper than it rests. Pushed to the surface, the
collar puffed; left alone, it sank up to 1.0 deeper than it rests in the attract movie's poses. What
still shows there, the poncho inside his chest and shoulders while he rides Dran, is the PS2's own: the
collar follows its rest place into the chest as his skinned chest and raised shoulders swell 0.3 and
more past their standing shape, Original clips the same, and neither a torso refitted every step nor a
limit of 0.3 under the surface changed it. The PS2's
step carries the cloth along with a share of the body's movement (`FOLLOW`) but none of its turning,
so a quick about-turn (15 degrees a tick, a dozen ticks) swung Toan's chest through the poncho. Carrying
the cloth round with turns faster than 3 degrees a tick, as current engines limit the angular inertia a
cloth feels, was tried first: it stopped the clipping but cut the poncho's movement against the body by
60% running in circles, and it looked stiff. The backstop keeps all of the PS2's movement (0.225 against
0.223 running in circles) and cut cloth inside the chest by 75 to 90%; on an about-turn the poncho still
swings round his side, round the chest instead of through it.

Everything else is the PS2's, so the cloth looks and moves as it always has, except that arms and legs
no longer pass through it.

A full position-based (XPBD) solver was tried first and dropped. Dark Cloud's cloth is tuned to the
PS2's soft, four-pass springs: they let the poncho stretch a quarter or more round the authored boxes,
many of whose rest places lie inside them. Held to its lengths, the poncho rode up off the body and
showed the belt; softened enough to hang right, it stretched off the neck, jittered at two and a half
times the PS2's rate and flipped up over Toan's face.

Both keep the vertices, speeds and normals in `CCloth`, so drawing is unchanged and the two can be
switched at any time. The Options screen's Display page has the Cloth row (Original, Enhanced);
Original is the default until both have been compared in play.

---

## 9. Phases

All four phases land in one pull request: nothing switches to glTF until everything does, so no
model, cloth included, ever draws without the behaviour it has today. The phases are the order the
work is done in and the gates it passes; each gate's tests (section 10) pass before the next phase
starts.

1. **Extraction.** Kazarov's model bridge and its configuration hook; Dark Cloud's command table and
   actions with their frames; the names added to the shared table; every model published as glTF,
   checked against the retail loaders on the disc.
2. **Characters from glTF.** The character, motion and model loaders build from the published glTF;
   every pose at every key frame equals retail's.
3. **The rest.** Static scenes, items, weapons, the title and opening, collision models and georama
   parts.
4. **Cloth.** The cloth documents; the original step from them; the enhanced mode and its Options
   row.

Later, measured, and separately: GPU skinning, glTF replacements that add actions or bones, and
unifying the glTF loader with ChronicleTwo's.

---

## 10. Tests

- The converter: Dark Cloud's `KEY` lines become named actions with their frames; the three-file
  `MOTION` form gives a skin; `TEX_ANIME` and `CLOTH` reach the sidecar; fixtures made in code.
- Parity on the disc (`DC_MODEL_DATA`, `DC_MODEL_RAW`): every model, weight and bind pose rebuilt
  from its bundle equals the disc's bytes; every character motion's channels, sampled at every frame
  of every action, match the disc's within 10⁻³, and no motion comes back with more keys than the
  disc's (the game sizes its arenas for them).
- The configuration reader: names from comments, missing commas, index-only comments, texture
  animations in order, cloth across lines, the game's argument checks.
- Loading: a character from glTF draws the same pose as from its pack at a set of frames; hit-window
  frames land on the same poses; `SearchFrame` finds every attachment point.
- Datapacks: a replacement glTF wins; a malformed one, or one missing an action, falls back.
- Cloth: the original step from the document matches today's `CCloth` step for step; the enhanced
  one keeps the cloth out of the arms and legs and otherwise moves as the original does.

---

## 11. Decided, and still open

Decided (r2):

- **Motion names** are translated into English for what the motion or model is for, and reviewed.
- **The glTF loader is Chronicle's own;** it is unified with ChronicleTwo's later.
- **Every model** uses the new format, georama parts and collision models included.
- **One pull request** for phases 1 to 4, which serve as test gates.

Decided (r3):

- **Kazarov takes a game profile;** the configuration syntax stays in Chronicle.
- **The game's loaders stay;** Chronicle rebuilds their records from the bundles.

Open:

- **The cloth default,** once Original and Enhanced can be compared.
- **Size.** Converted models take 1.9 GB before packing, against 650 MB on the disc, mostly from
  motion packs placed into every model they fit (as for Dark Cloud 2). Each could instead live once,
  for Dark Cloud's motions apply by frame index.
- **Stripping by default.** `extract` keeps the disc's files beside the glTF for now; stripping is
  a separate step until loading from glTF is proven in play.

---

## 12. Status

- **Phase 1, extraction:** done. `extract` converts the models with the textures;
  `dcdata convert-models <raw> <out>` converts the disc into a folder of its own.
- **Phase 2, loading:** on the PAL disc, every model, weight and bind pose rebuilds byte for byte,
  and every one of the 2,450 motions keeps its poses in every action (largest difference 9×10⁻⁶),
  never with more keys than the disc's. The loader rebuilds every source as its file is read.
  `dcdata bake-assets <raw> <data>` publishes the models into a data folder; baked again over its
  own output, it leaves out what it published before (`.assets` folders, glTF, buffers, sidecars,
  `models.json`), which it would otherwise publish again beside the new files. Tested in play
  in Norune and the attract movie.
- **Phase 3, the rest:** every model source on the disc is indexed (11,949: scenes, georama `.scn`
  slots, dungeon `.mpd` parts, the opening's `.sne`, title and menu packs, collision models, items,
  weapons, cutscene motion sets). `dcdata strip-models <data>` empties the converted sources in
  place; stripped data reads back as the disc's.
- **Phase 4, cloth:** Enhanced (the PS2's step plus the body) and its Options row. Tests: on a
  skeleton, Enhanced keeps the cloth out of a leg the Original passes through; an arm pushes cloth out
  of it, presses it only as far as the chest and never deeper than it rests, and leaves cloth that
  rests against it alone; the chest keeps cloth off itself, and cloth authored inside it no deeper than
  it rests; the torso fits a body's mesh, deep chest and shallow back, without a hand at its side
  widening it. In headless runs of Toan running, turning and stopping in Norune, Enhanced moves
  as the Original does (same tick-to-tick motion, neck stretch and hem) while the limbs push the
  poncho hundreds of times; through quick about-turns and running in circles the poncho swings as on
  the PS2 but round his chest rather than through it.
- **Later:** datapack models (section 6), the game reading cloth from its typed commands, GPU
  skinning, and unifying the loader with ChronicleTwo's.

**Safeguards.** Each source's size and CRC-32 are in `models.json`. The loader writes a rebuilt
source only over the bytes it was converted from, or the zeros stripping left of them, so a pack a
mod changed keeps what it has. A rebuilt model, weight or bind pose must match its CRC, or the file
keeps what it read. A motion keeps the disc's key frames in every channel: the game reads every
vertex channel of a frame at the first one's key, and blends towards keys by frame. The channels'
order matters for the same reason: rebuilt sorted by frame, Saget's vertex animation in the attract
movie, read against the wrong first channel, wrote past his mesh and crashed its draw. So the sidecar
lists every channel of a motion in its order, with its header words (the same three in every channel
of a file: tool leftovers), and keeps with their words the keys a clip can't give back bit for bit
(glTF's unit rotations change the last bit of a disc rotation that wasn't quite one); every motion
rebuilds as the disc has it, and one that doesn't goes into a pack only if it matches the disc's CRC.
`ModelGltf.MotionsGoInOnlyAsTheDiscHasThem` checks every motion in every pack. Stripped data
(`models.stripped`) refuses to run, with a message to rebake, when its index is missing or a
source can't be rebuilt, rather than handing the game zeros.
