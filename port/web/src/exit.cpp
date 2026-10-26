// _Exit on the web. std::_Exit from the game's pthread waits on the main thread to exit, which keeps the runtime
// alive for that very pthread (PROXY_TO_PTHREAD), so it never returns and the page never hears the status. This
// ends the process as _Exit means to: the page's onExit gets the status and the threads are stopped, with no
// static destructors run.

#include <emscripten/em_asm.h>

#include <chrono>
#include <thread>

[[noreturn]] void WebQuickExit(int status) {
    MAIN_THREAD_ASYNC_EM_ASM({
        EXITSTATUS = $0;
        ABORT = true;
        Module['onExit']?.($0);
        PThread.terminateAllThreads();
    }, status);
    // This thread is among those the main thread stops.
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
