# The web build

DC_Web builds Chronicle's port to WebAssembly with Emscripten (`PLATFORM=WEB`, preset `web`). This page
records how it is built and where it stands.

## Building

```bash
source /path/to/emsdk/emsdk_env.sh   # emsdk 6.0.13, as CI pins
cmake --preset web
cmake --build --preset web
cd port/build/web && python3 -m http.server   # then open http://localhost:8000/
```

The build needs `glslangValidator` on the host (it still compiles the Vulkan shaders). Output is
`port/build/web/darkcloud.js`, `darkcloud.wasm` and `darkcloud.data` (`lang/` and `glyphs/`, preloaded), the
game drawing with WebGPU, and `darkcloud_gl.js`, `.wasm` and `.data`, the same game drawing with WebGL2, with
the page (`port/web/site/`) beside them, so the build directory is the site.

The `web` workflow (`.github/workflows/web.yml`) builds every push to `main` and `claude/**`, smoke-runs the
build under Node and the page in headless Chromium, and force-pushes the log, the error summary, both smoke
logs (`smoke.log`, `browser.log`) and the build to the `ci-logs` branch.

## What the web build changes

All of it is in `port/web/` or behind `__EMSCRIPTEN__` / `EMSCRIPTEN`:

- **SDL3** is built from source (release-3.4.18).
- **Vulkan on WebGPU** (`port/web/vulkan/`): browsers have no Vulkan, so the Vulkan commands Kazarov's
  renderer calls are implemented on WebGPU (Emscripten's `emdawnwebgpu` port) and the renderer runs
  unchanged. See "Vulkan on WebGPU" below. Weak stubs that fail (`gen_vk_stubs.py`) stand for every
  other command.
- **Vulkan on WebGL2** (`port/web/gl/`): the same commands on WebGL2, linked into `darkcloud_gl`, which the page
  starts where WebGPU is missing or draws the game wrong. See "Vulkan on WebGL2" below. Each layer is an
  object library, so the stubs' weak definitions never stand in for its commands.
- **PS2 threads** (`sce/eekernel_threads.cpp`) are Emscripten fibers, which need `-sASYNCIFY`.
- **The game runs on a pthread** (`-sPROXY_TO_PTHREAD`), so its blocking waits do not stall the page.
  Hosting needs cross-origin isolation (COOP/COEP headers, or a service worker on GitHub Pages). The
  canvas is handed to that thread as an OffscreenCanvas, and the renderer stays on it (render thread off):
  WebGPU's objects belong to the thread that made the device.
- **Arenas** are heap blocks (`port/web/src/memory.cpp`); WebAssembly has no page protection, so the
  guard page after each catches nothing.
- **`u_long` is 64-bit**, as on Windows: wasm32's `long` is 32-bit and the GS register bitfields need 64.
- **`main`** has the standard two-argument signature on the web.
- **Exceptions** are Emscripten's JavaScript-based ones (`-fexceptions`); native wasm exceptions do not
  work with Asyncify.
- Discord is stubbed, as on the Switch.
- **Game data** is the player's own, from their PAL disc. The page copies what they choose into the origin
  private file system under `chronicle/` once: the disc image (`disc.iso`), the disc's `DATA.DAT` and
  `DATA.HD2` (`disc/`), an extracted `data` folder (`data/`), or the `data.zip` dcdata packs. The game mounts
  that file system at `/opfs` through WasmFS (`port/web/src/storage.cpp`) and starts with
  `--data /opfs/chronicle/data --save /opfs/chronicle/save`; saves persist there. A disc is read in place
  (`DataArchive::OpenDisc`, `platform/data_archive.cpp`): each file of `DATA.HD2` from `DATA.DAT` a seek at a
  time, with the data folder's loose files over them. At its first start the game makes those loose files, what
  dcdata writes beside a disc's (`normalized/`, `data.hd2`, `languages.json` and the aliases), from the
  menus, system packs and other files they come from, copied into a WasmFS memory directory rather than the
  browser's storage, where making a file takes seconds on a phone (`port/web/src/firstrun.cpp`);
  `languages.json`, written last, marks them done.
  Copying every file of the disc to the browser's storage took many minutes on a phone. `data.zip` is read in
  place the same way. OPFS needs Chromium or Firefox; Safari lacks `createWritable` on the page.
- **Render resolution**: the game draws at half the window's resolution on a touch screen, as Android's port
  starts (a phone's GPU filling all its screen's pixels fell to about 47 frames a second in town), and at the
  whole of it elsewhere; Options' Render Resolution changes it (`video.render_scale`, 0 for that choice).
- **On-screen gamepad**: on a touch screen (`pointer: coarse`) the page draws a PS2 pad over the game, D-pad,
  face and shoulder buttons, Select, Start and both sticks, and hands its state to the game the way Android's
  Java overlay does (`port/web/src/touch.cpp`, `KZ_TOUCH_OVERLAY`). The game hides it while a gamepad is
  connected or a key held, and the Touch Opacity option sets how visible it is.
- **Saves**: Kazarov's `FilesWrite` commits a new file with an exclusive rename or a hard link, neither of
  which WasmFS has; on the web it checks the name is free and renames (`platform/files.cpp`).
- **The page** (`port/web/site/`): `shell.js` stores the data and starts the game, which then fills the
  window, scaled to fit, with Fullscreen and Log buttons and the first start's progress over it; `sw.js` is a
  service worker that adds COOP/COEP for hosts that cannot send them, GitHub Pages among them, and
  `shell.js` reloads once under it. `?args=--headless+--frames+30` on the URL passes extra arguments.
  The page starts `darkcloud_gl.js` on Arm's Mali GPUs (by WebGL's name for the GPU) and where there is no
  WebGPU, and `darkcloud.js` elsewhere; `?renderer=gl` or `?renderer=webgpu` chooses, and the log's first lines
  say which ran on what GPU.
- **MoonMod** (github.com/Jeffiest/Moonmod): its patches (`ports/chronicle/moonfactory-chronicle.patch` and
  `moonfactory-kazarov.patch`) are applied here, Lua runtime and all, so mods load as on the desktop. The
  game looks for them in `save/mods/`, which is `chronicle/save/mods/` in the browser's storage; the page's
  Mods card adds a mod's folder, a folder of mods or a `.zip` of either there (`zip.js` reads a zip a file at a
  time), and removes them. Before the game starts it turns each mod on or off and changes its settings, all in
  MoonMod's profile (`save/moonmod/profile.json`, which the game's Mods menu also writes); "Start the game with
  mods" off starts it with `MOONMOD_NO_MODS=1`. Native extensions (DLLs) do not load on the web.
- **Built-in mods.** Every Chronicle mod in MoonMod's `mods/`, at the commit `port/web/mods/builtin.json` pins,
  comes with the page: `mods/builtin.py` (the `dc_web_builtin_mods` target) zips each into the build's
  `mods/builtin/<id>.zip` with `index.json` listing them. MoonMod is cloned into the build directory once;
  `-DDC_MOONMOD_DIR=<checkout>` reads the commit from a local checkout instead. The page installs them into
  `save/mods/` when it finds the game data, off until the player turns them on, as the game leaves a new mod, and
  marks them "built in" on the Mods card. `save/moonmod/builtin.json` records each one's installed version, so a
  newer pin is copied over the old folder (files the player saved into it, such as the animation editor's motions,
  stay); a built-in mod the player removes stays removed until "Restore the built-in mods", and one they add
  themselves is theirs and left alone. `moonmod-mods.yml` moves the pin to MoonMod's main each hour its `mods/`
  folder has changed, and builds the page; a pin set by hand works too.
- **Online play** (`net.*`, `coop/relay`): the browser's WebSocket (Emscripten's `websocket.h`, `-lwebsocket.js`,
  `port/src/moonmod/net_transport_web.cpp`) to the co-op relay. The socket lives on the page's main thread and queues
  frames for the game's thread. The relay is a Cloudflare Worker (`coop/relay/README.md`), deployed by
  `coop-relay.yml`; PC builds reach the same relay, so web and PC players share rooms.
- **Play on your PC.** The page offers the game for the player's own computer: the newest `build-<commit>`
  release with a file for the system they pick (found through GitHub's API; its releases page if that can't be
  asked), and the built-in mods for it, all in `mods/builtin/darkcloud-mods.zip` or each on its own. `builds.yml`
  attaches the same `darkcloud-mods.zip` to each build release and marks the release of main's newest commit as
  GitHub's Latest.
- **Mods for the PC.** The page lists mods for players running the game on their own computer
  (`site/mods/catalog.json`), each version with the build it was confirmed with, and offers both for the
  player's system. Both are this repository's releases:
  - `build-<commit>`: each of main's commits that `pc.yml` and `macos.yml` both pass, with the Windows zip, the
    Flatpak and the macOS archive (`builds.yml`).
  - `mod-<id>-<version>`: the mod's folder zipped from the MoonMod commit the list names (`mods.yml`). A version's
    zip never changes, so a changed mod needs a new version.

  To list a mod, play it with a build of main, then, from a MoonMod checkout at the commit to publish (pushed):

      python port/web/mods/catalog.py add --moonmod <checkout> --mod <id> --build <main commit>

  and merge the change to the list; `mods.yml` publishes the zip. `check` checks the list, and
  `publish --dry-run` makes the zips without releasing them.
- **The animation editor** (`site/anim/`) is MoonMod's (`tools/anim/editor`), ported by
  `port/web/anim/sync_editor.py`: `anim/data.js` answers what the desktop's `server.py` does, from the
  player's game data in the browser's storage. A disc's character and monster packs are found through
  `DATA.HD2` (and an image's ISO 9660 root), as dcdata finds them. Their models are the glTF dcdata's
  converter makes: a data folder or `data.zip` already holds them, and for a disc `anim/convert-worker.js`
  runs `dc_anim_convert` (`port/web/anim/convert.cpp`, dcdata's model step with Kazarov's Rust core built
  for `wasm32-unknown-emscripten`) on the one pack and keeps the result under `chronicle/anim/`. Rust's
  standard library has no threads there, so Kazarov's pools are the calling thread (`core.rs`,
  `thread_pool`). Motions save into a stored mod (`mods/<id>/resources/anims/...`, as on the desktop) or
  download. The Reference panel needs the desktop's files and stays hidden.

## Status

| Stage | State |
| :-- | :-- |
| Compiles and links to wasm | Done |
| Game data in the browser: disc image, disc files, data folder or `data.zip` | Done; a disc read in place |
| Renderer on WebGPU (Vulkan on WebGPU) | Done; Kazarov's renderer tests pass in Chromium, the game plays in Chrome |
| Renderer on WebGL2 (Vulkan on WebGL2) | New; CI runs Kazarov's renderer tests on it; for Android's Mali GPUs |
| Saves | Done |
| Page: fills the window, fullscreen, log drawer | Done |
| Main-depth copies | Not yet (see below) |
| Hosting on GitHub Pages | Deploy job ready, from `main` |
| Dark Chronicle beside it | Built by `build-dc2` from a pinned commit, published at `dc2/`, a row of games on both pages |

## Dark Chronicle on the same site

The site offers two games: Dark Cloud at its root and Dark Chronicle, ChronicleTwo's web build, at `dc2/`.

- **The build.** `port/web/dc2.json` names the repository and the commit. `web.yml`'s `build-dc2` checks that commit
  out, builds `darkchronicle` (WebGPU) and `darkchronicle_gl` (WebGL2) with its `tools/web/build.sh`, and keeps the
  page and the two games in a cache under the commit, so only a change of the pin compiles the game again (an hour
  or more on a cold compiler cache; the first run on `main` after a branch moved the pin is such a run, since a
  branch's cache is not `main`'s). To publish a newer Dark Chronicle, push it to that repository and put its commit
  in `dc2.json`. What it builds from is that repository's tracked files only: fonts, scripts and the gameplay seed
  go into `darkchronicle.data`, no game data.
- **The row of games.** Each page's `shell.js` (`showGames`) reads a `games.json` beside it,
  `[{"name": "Dark Cloud", "href": "./"}, {"name": "Dark Chronicle", "href": "dc2/"}]`, and shows the entries as
  links above the title, the one for its own folder marked. `deploy` writes one for each page. A build served
  alone has none and shows no row.
- **One origin.** The two pages share the browser's storage for the site: Dark Cloud keeps `chronicle/` and
  `moonmod/` there, Dark Chronicle `chronicletwo/`, and their page keys differ (`dc-` and `ct-`), but the space
  both may use is one allowance. Each folder has its own `sw.js`, the same isolating worker.
- **Deploying.** `deploy` needs all three builds. One that fails publishes nothing, so the site stays as it was.

## Vulkan on WebGPU

`port/web/vulkan/` implements the Vulkan commands the renderer calls, and only those, on `webgpu.h`. It
reports a Vulkan 1.3 portability-subset device, the same path the renderer takes on Apple through MoltenVK:
push descriptors instead of the bindless array, no triangle fans, one stencil reference and mask pair. It
offers no timestamps, depth bounds or depth blits.

- **Commands** encode into a WebGPU command encoder as they are recorded. Dynamic rendering is a render
  pass. WebGPU has no dynamic state, so the command buffer tracks Vulkan's and each draw picks a variant of
  its pipeline by that state and the pass's formats; `vkCreateGraphicsPipelines` only keeps a description.
- **Push constants** are copied per draw into a storage buffer read at a dynamic offset (bind group 2).
- **Memory**: host-visible memory is a copy in wasm memory. The renderer writes it only through its
  transient allocator (`gfx/memory.cpp`), which marks each span it hands out; the marked ranges are
  uploaded when they are next submitted.
- **The swapchain** is three textures of the layer's own; `vkQueuePresentKHR` copies the frame to the canvas's
  current texture and yields. The canvas's texture is shown whenever the thread yields, as the renderer's fence
  waits do mid-frame on a slow GPU, so frames drawn into it directly flashed on phones. A second pass then sets
  the canvas's alpha to 1: the frame's alpha is the GS frame buffer's, which an opaque canvas should ignore,
  but Android on Mali flickered to frames as much brighter as a colour divided by it.
- **Copies** WebGPU cannot take as they are, rows not a multiple of 256 bytes, go through an upload buffer
  with padded rows; readbacks land in map-read buffers that the fence wait unpacks. Blits that scale or
  flip, attachment clears and colour image clears are drawn by the layer's own small pipelines.
- **Shaders** are hand-ported to WGSL (`vulkan/shaders/`) and matched to the renderer's SPIR-V modules
  word for word. Specialisation constants are WGSL overrides with the same ids; clip space is flipped in y.
  When a GLSL shader in `gfx/shaders/` changes, its WGSL port must follow.
- **Mali GPUs** (Android phones) showed frames on the canvas before the GPU had finished drawing them: the copy
  to the canvas outran the frame's last draws, as flicker and texel-sized blocks: on Mali a submission can read
  what the one before it has not finished writing, though not inside one submission. On an Arm adapter a frame's
  submissions (the game's and the display copy's) are held and go to the GPU together at present, each frame's
  first waits for the frames before it, and the canvas gets the newest finished frame. The log tells what the
  waits cost every 600 frames. `DC_WEBGPU_BATCH`, `DC_WEBGPU_ONE_FRAME` and `DC_WEBGPU_PRESENT_FINISHED`, `1` or
  `0` with `?args=--env+...`, force the choices; CI runs the GPU renderer tests a second time with all three.
- **Without clip distances** (Mali GPUs, among others), every shader comes in a `_noclip` variant: the vertex
  shader hands its clip distances to the fragment shader as two more varyings, and that discards where any is
  negative. CI runs the GPU renderer tests a second time on it, as on Mali (`--no-clip-distances --mali`).
- **Without dual-source blending** (SwiftShader, some browsers), the blends that use a second source,
  only ever on targets without alpha, draw from one source: the shader writes the factor as alpha and the
  pipeline leaves alpha out of the target.

Headless Chromium on SwiftShader, which CI uses, loses the WebGPU device once a canvas presents, so CI
runs the game `--offscreen` and checks pixels through readbacks. A browser with a GPU does not.

Kazarov's renderer tests (`gfx_*_test.cpp`, built as `kazarov_gfx_tests` and run in Chromium by CI) are the
layer's conformance suite. Left out of the run:

- `GfxGpuDepthCopy.*`: main-depth copies (`CopyMainDepthToTarget`) are not offered yet, as WebGPU cannot blit
  depth. The game then draws the depth-of-field haze without its low-resolution copy.
- `GfxGpuMeshDc2.FogCrossingTheEyeBlendsClampedFog` and `PlainFogCrossesTheEyeWithoutDividingByZero`: their
  vertices lie exactly on the eye plane (w = 0), where SwiftShader's clipper leaves those vertices out of the
  interpolation. A hair either side of the plane, and on other GPUs, the result is the expected one.

## Vulkan on WebGL2

`port/web/gl/` implements the same commands on WebGL2 (OpenGL ES 3.0) and reports the same device as Vulkan on
WebGPU, so the renderer takes the same path on either. It is for Android's Mali GPUs, whose WebGPU flickered
until frames were made to wait for each other, which halved the frame rate (above): WebGL orders the work it is
given itself, and nothing waits.

- **Commands** are recorded with the state each draw needs, and replayed as GL calls at submission
  (`commands.cpp`), so a fence is signalled once `vkQueueSubmit2` returns and readbacks (`glReadPixels`) have
  landed by then. GL state is set only where it differs from what was set last.
- **Memory**: every buffer's bytes are in wasm memory, host-visible memory's own and a device-local buffer's
  copy. Buffers drawn from have GL buffers, one per target, as WebGL keeps an index buffer from being anything
  else; what the transient allocator marked or a copy wrote is uploaded before a draw next reads it. Buffer
  copies happen in wasm memory.
- **Uniform blocks** stand for push constants and the storage buffers' records: each draw's push constants and
  the `MeshConstants`, `Prog0Constants` and `Prog0Palette` records it reads are copied into one uniform buffer
  per submission, records shared by draws once.
- **Images** keep Vulkan's layout: GL draws into a framebuffer row 0 first as Vulkan does, so clip space keeps
  its y, and only the front-facing winding and the clip-space depth (0 to w in Vulkan, -w to w in GL) differ. The
  canvas's row 0 is at its bottom, so the frame is blitted to it flipped at present. Copies and blits are
  framebuffer blits; depth, which WebGL does not read back, is drawn into a colour target as its bytes first.
- **Shaders** are hand-ported to GLSL ES 3.00 (`gl/shaders.cpp`), as the WGSL ports are: specialisation
  constants are `#define`s, clip distances discard in the fragment shader, `textureGather` is four nearest
  samples, and dual-source blends draw from one source as without WebGPU's. When a GLSL shader in
  `gfx/shaders/` changes, its GLSL ES port must follow too.

CI runs the renderer tests on it as well (`gfx_tests.html?renderer=gl`, `kazarov_gfx_tests_gl`).

## Watch for

wasm-ld turns a call whose type differs from its definition into a trap, where native linkers let it
through. CI lists any "function signature mismatch" in `errors.txt`; each one is a crash waiting at that call.
