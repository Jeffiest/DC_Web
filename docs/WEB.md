# The web build

DC_Web builds Chronicle's port to WebAssembly with Emscripten (`PLATFORM=WEB`, preset `web`). This page
records how it is built and where it stands.

## Building

```bash
source /path/to/emsdk/emsdk_env.sh   # emsdk 6.0.13, as CI pins
cmake --preset web
cmake --build --preset web
```

The build needs `glslangValidator` on the host (it still compiles the Vulkan shaders). Output is
`port/build/web/darkcloud.js` and `darkcloud.wasm`, with `lang/` and `glyphs/` beside them.

The `web` workflow (`.github/workflows/web.yml`) builds every push to `main`, smoke-runs the build under
Node, and force-pushes the log, the error summary, the smoke log and the build to the `ci-logs` branch.

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

## Status

| Stage | State |
| :-- | :-- |
| Compiles and links to wasm | Done |
| Starts under Node up to the game-data check | Done |
| Runs game logic with real data (headless) | Not yet tested: needs extracted data |
| Game data in the browser (drop `data.zip`, keep it in OPFS) | Not started |
| WebGPU renderer behind `gfx.hpp` | Not started |
| Audio, input, page and hosting | Not started |

## Watch for

wasm-ld turns a call whose type differs from its definition into a trap, where native linkers let it
through. CI lists any "function signature mismatch" in `errors.txt`; each one is a crash waiting at that call.
