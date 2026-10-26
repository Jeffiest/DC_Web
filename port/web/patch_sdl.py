#!/usr/bin/env python3
"""Fixes SDL3's dummy audio driver on Emscripten (applied to the fetched SDL by web.cmake).

Its timer calls SDL_PlaybackAudioThreadIterate, which returns bool, as dynCall('vi', ...); WebAssembly traps on
the signature mismatch at every tick, so a headless run, which uses the dummy driver, never mixes. The call is
made 'ii'. Running it again changes nothing.

usage: patch_sdl.py <SDL source dir>
"""

import sys
from pathlib import Path


def main() -> int:
    path = Path(sys.argv[1]) / "src" / "audio" / "dummy" / "SDL_dummyaudio.c"
    text = path.read_text(encoding="utf-8")
    patched = text.replace("dynCall('vi', $3, [$4]);", "dynCall('ii', $3, [$4]);")
    if patched != text:
        path.write_text(patched, encoding="utf-8")
    elif "dynCall('ii', $3, [$4]);" not in text:
        print("patch_sdl: SDL_dummyaudio.c no longer has the call to fix; check the patch", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
