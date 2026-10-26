# macOS on Apple Silicon

The PC port (`docs/PC.md`) builds natively for arm64 macOS and runs on Vulkan
through MoltenVK. Like on Linux, it needs your own PAL disc.

## Dependencies

Xcode or its command line tools, and Homebrew in `/opt/homebrew`:

```sh
brew install llvm lld cmake ninja python glslang sdl3 nlohmann-json vulkan-headers vulkan-loader vulkan-tools molten-vk googletest
```

The build also needs Rust 1.99.0 (`rustup toolchain install 1.99.0 --profile minimal`), `cxxbridge-cmd` 1.0.205
(`cargo install cxxbridge-cmd --version 1.0.205 --locked`) and a static FFmpeg from
`python3 tools/kazarov/scripts/build/kz-ffmpeg --out .kazarov/ffmpeg/darwin-arm64`.

The presets use Homebrew's `llvm`; Apple's clang is not current enough for C++26.

## Building

```sh
./build.sh macos-arm64                        # Debug, port/build/macos-arm64
./build.sh macos                              # Release, port/build/macos-arm64-release
cmake --preset macos-arm64-release            # the same Release build, without the container
cmake --build --preset macos-arm64-release
```

`CLEAN=1` discards the build directory first; `JOBS=N` sets the number of
parallel jobs.

## Game data

```sh
port/build/macos-arm64-release/dcdata extract "rom/pal/Dark Cloud (PAL).iso" data
port/build/macos-arm64-release/dcdata pack data
```

`pack` writes `data.zip` beside `data/`. The game accepts the archive on its own and gives it priority when both are present.

Without it, the first windowed start asks for the disc image and extracts it.
File names on the disc that are not UTF-8 are written with `%xx` escapes, since
APFS refuses them; the game looks them up the same way.

## Running

```sh
export VK_DRIVER_FILES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json
port/build/macos-arm64-release/darkcloud --data data --save save
```

The Vulkan loader does not search Homebrew's prefix, so `VK_DRIVER_FILES` is
required. Options and `config.json` are those of `docs/PC.md`.

## Tests

The tests run on MoltenVK too, as CI runs them:

```sh
export VK_DRIVER_FILES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json
ctest --preset macos-arm64
```
