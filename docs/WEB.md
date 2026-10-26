# The web build

`PLATFORM=WEB` (the `web` preset) builds the port to WebAssembly with Emscripten. The `web` workflow builds it
on every push to `main` and `claude/**` and force-pushes the results to the `ci-logs` branch: `build.log`,
`errors.txt`, `smoke.log` (the build headless under Node), `browser.log` (the page in headless Chromium) and
the build itself.

```sh
emcmake cmake --preset web   # or: source emsdk_env.sh; cmake --preset web
cmake --build --preset web
cd port/build/web && python3 -m http.server   # then open http://localhost:8000/
```

## State

| Stage | State |
| --- | --- |
| Game and Kazarov compile and link to wasm | Done |
| PS2 threads as Emscripten fibers (Asyncify), game on a pthread | Done; runs to the data check under Node |
| Game data from the browser's storage (OPFS) | This stage |
| Renderer | Vulkan stubs that fail (`port/web/gen_vk_stubs.py`); needs a WebGPU backend behind `gfx.hpp` |
| Audio, input | SDL3; untested |
| Hosting | The build directory is the site; GitHub Pages needs the repository public (or a paid plan) |

## How it fits together

- **Threads.** The game runs on a pthread (`-sPROXY_TO_PTHREAD`) so its blocking waits never stall the page,
  and the PS2 threads are Emscripten fibers (`sce/eekernel_threads.cpp`). Threads need `SharedArrayBuffer`,
  which needs a cross-origin isolated page. Hosts that cannot send COOP/COEP headers, GitHub Pages among them,
  get them from `site/sw.js`, a service worker that `site/shell.js` registers before reloading once.
- **Data.** The player brings their own data: the `data.zip` that `dcdata` makes from their disc on the
  desktop. `shell.js` copies it into the origin private file system as `chronicle/data.zip` once. The game
  mounts that file system at `/opfs` through WasmFS (`port/web/src/storage.cpp`) and starts with
  `--data /opfs/chronicle/data --save /opfs/chronicle/save`, so the archive is read in place a seek at a time,
  never loaded whole into memory, and saves persist. OPFS needs Chromium or Firefox; Safari lacks
  `createWritable` on the page.
- **Bundled files.** `lang/` and `glyphs/`, which the game reads beside its executable, are preloaded into
  `darkcloud.data`.
- **Linking.** `wasm-ld` has no `--defsym`, so the two aliases the other platforms make at link time are
  appended to their units (`port/web/aliases`). A call whose type differs from its definition becomes a trap
  in wasm, so the workflow reports `signature mismatch` warnings.
- **Arguments.** `?args=--headless+--frames+30` on the page's URL passes extra arguments to the game.

## Next

1. A WebGPU backend behind `tools/kazarov/src/cpp/gfx/gfx.hpp`, with the GLSL shaders translated to WGSL.
2. Audio and input checked in a browser.
3. Extraction in the page, from the disc image, if `dcdata`'s C++ path builds to wasm without its Rust and
   FFmpeg parts.
