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
`port/build/web/darkcloud.js`, `darkcloud.wasm` and `darkcloud.data` (`lang/` and `glyphs/`, preloaded),
with the page (`port/web/site/`) beside them, so the build directory is the site.

The `web` workflow (`.github/workflows/web.yml`) builds every push to `main` and `claude/**`, smoke-runs the
build under Node and the page in headless Chromium, and force-pushes the log, the error summary, both smoke
logs (`smoke.log`, `browser.log`) and the build to the `ci-logs` branch.

## What the web build changes

All of it is in `port/web/` or behind `__EMSCRIPTEN__` / `EMSCRIPTEN`:

- **SDL3** is built from source (release-3.4.18).
- **Vulkan**: browsers have none. The renderer compiles against Vulkan-Headers 1.4.366 and links
  `gen_vk_stubs.py`'s stubs, which fail, so `RendererInit` cannot start yet. A WebGPU backend behind
  `gfx.hpp` replaces this.
- **PS2 threads** (`sce/eekernel_threads.cpp`) are Emscripten fibers, which need `-sASYNCIFY`.
- **The game runs on a pthread** (`-sPROXY_TO_PTHREAD`), so its blocking waits do not stall the page.
  Hosting needs cross-origin isolation (COOP/COEP headers, or a service worker on GitHub Pages).
- **Arenas** are heap blocks (`port/web/src/memory.cpp`); WebAssembly has no page protection, so the
  guard page after each catches nothing.
- **`u_long` is 64-bit**, as on Windows: wasm32's `long` is 32-bit and the GS register bitfields need 64.
- **`draw_rect` and `WorkBuffer__2`** are aliases appended to `ebattle.cpp` and `dataset.cpp`
  (`port/web/aliases/`), because wasm-ld has no `--defsym`.
- **`main`** has the standard two-argument signature on the web.
- **Exceptions** are Emscripten's JavaScript-based ones (`-fexceptions`); native wasm exceptions do not
  work with Asyncify.
- Discord and the first-run extractor are stubbed, as on the Switch.
- **Game data** is the player's own: the `data.zip` that `dcdata` makes from their disc on the desktop.
  The page copies it into the origin private file system as `chronicle/data.zip` once. The game mounts
  that file system at `/opfs` through WasmFS (`port/web/src/storage.cpp`) and starts with
  `--data /opfs/chronicle/data --save /opfs/chronicle/save`, so the archive is read in place a seek at a
  time, never loaded whole into memory, and saves persist. OPFS needs Chromium or Firefox; Safari lacks
  `createWritable` on the page.
- **The page** (`port/web/site/`): `shell.js` handles the data and starts the game; `sw.js` is a service
  worker that adds COOP/COEP for hosts that cannot send them, GitHub Pages among them, and `shell.js`
  reloads once under it. `?args=--headless+--frames+30` on the URL passes extra arguments to the game.

## Status

| Stage | State |
| :-- | :-- |
| Compiles and links to wasm | Done |
| Starts under Node up to the game-data check | Done |
| Runs game logic with real data (headless) | Not yet tested: needs extracted data |
| Game data in the browser (drop `data.zip`, keep it in OPFS) | Built; checked in Chromium by CI |
| WebGPU renderer behind `gfx.hpp` | Not started |
| Page | Done |
| Audio, input and hosting | Not started |

## Watch for

wasm-ld turns a call whose type differs from its definition into a trap, where native linkers let it
through. CI lists any "function signature mismatch" in `errors.txt`; each one is a crash waiting at that call.
