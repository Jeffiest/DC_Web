// The page's persistent storage (docs/WEB.md): the browser's origin private file system mounted at /opfs.
// The page copies the player's data.zip to chronicle/data.zip there once and starts the game with
// --data /opfs/chronicle/data --save /opfs/chronicle/save, so the archive is read in place, a seek at a
// time, and saves outlive the tab. Node, where the smoke run goes, has no OPFS and keeps WasmFS's memory.

#include <emscripten/em_asm.h>
#include <emscripten/wasmfs.h>

#include <cstdio>

void WebMountStorage() {
    const bool available = EM_ASM_INT({
        return typeof navigator !== 'undefined' && !!navigator.storage && !!navigator.storage.getDirectory;
    });
    if (!available) {
        return;
    }
    // The OPFS backend works only off the browser's main thread; main runs on a pthread (PROXY_TO_PTHREAD).
    backend_t opfs = wasmfs_create_opfs_backend();
    if (wasmfs_create_directory("/opfs", 0777, opfs) != 0) {
        std::fprintf(stderr, "cannot mount the browser's storage at /opfs\n");
    }
}
