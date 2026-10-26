# The browser build (PLATFORM WEB), included by port/CMakeLists.txt with Emscripten's toolchain.
#
# This first stage builds the game and Kazarov's host code to WebAssembly unchanged where it can:
# - SDL3 is built from source for Emscripten.
# - Browsers have no Vulkan. The renderer compiles against Vulkan's headers and links stubs that fail
#   (gen_vk_stubs.py), until it gets a WebGPU backend; RendererInit cannot start yet.
# - The PS2 threads are Emscripten fibers (sce/eekernel_threads.cpp), which need Asyncify.
# - The game runs on a pthread (PROXY_TO_PTHREAD), so its blocking waits do not stall the page.

set(DC_WEB_DIR ${CMAKE_CURRENT_LIST_DIR})
include(FetchContent)

# Every unit, the libraries fetched below included, shares the thread and exception model.
add_compile_options(-pthread -fexceptions)
add_link_options(-pthread -fexceptions)

# SDL3, static.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.18
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL3)
if(NOT TARGET SDL3::SDL3)
    add_library(SDL3::SDL3 ALIAS SDL3-static)
endif()

# Vulkan's headers, and a stub for every command in place of a loader.
FetchContent_Declare(VulkanHeaders
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG v1.4.366
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(VulkanHeaders)
set(DC_VK_STUBS ${CMAKE_BINARY_DIR}/web/vk_stubs.cpp)
add_custom_command(OUTPUT ${DC_VK_STUBS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/web
    COMMAND ${Python3_EXECUTABLE} ${DC_WEB_DIR}/gen_vk_stubs.py
        ${vulkanheaders_SOURCE_DIR}/include/vulkan/vulkan_core.h ${DC_VK_STUBS}
    DEPENDS ${DC_WEB_DIR}/gen_vk_stubs.py
    COMMENT "Generating the Vulkan stubs"
    VERBATIM)
add_library(dc_vk_stubs STATIC ${DC_VK_STUBS})
target_link_libraries(dc_vk_stubs PUBLIC Vulkan::Headers)
target_compile_options(dc_vk_stubs PRIVATE -Wno-unused-parameter)
add_library(Vulkan::Vulkan INTERFACE IMPORTED GLOBAL)
set_target_properties(Vulkan::Vulkan PROPERTIES INTERFACE_LINK_LIBRARIES dc_vk_stubs)

# Kazarov's host library with the arena memory replaced: Emscripten's mmap has no reservations or guard pages.
function(dc_web_host target)
    set_source_files_properties(${KZ_PORT_ROOT}/platform/memory.cpp PROPERTIES HEADER_FILE_ONLY ON)
    target_sources(${target} PRIVATE ${DC_WEB_DIR}/src/memory.cpp)
endfunction()

# The desktop-only units, replaced.
function(dc_web_port target)
    target_sources(${target} PRIVATE ${DC_WEB_DIR}/src/discord.cpp ${DC_WEB_DIR}/src/firstrun.cpp)
endfunction()

# wasm-ld has no --defsym. The two names the other platforms alias at link time are aliases appended to
# the units that define their storage.
function(dc_web_alias_footer unit copy)
    if(unit STREQUAL "ebattle.cpp")
        set(footer ${DC_WEB_DIR}/aliases/ebattle.inc)
    elseif(unit STREQUAL "dataset.cpp")
        set(footer ${DC_WEB_DIR}/aliases/dataset.inc)
    else()
        set(DC_WEB_FOOTER "" PARENT_SCOPE)
        set(DC_WEB_FOOTER_COMMAND "" PARENT_SCOPE)
        return()
    endif()
    set(DC_WEB_FOOTER ${footer} PARENT_SCOPE)
    set(DC_WEB_FOOTER_COMMAND COMMAND ${Python3_EXECUTABLE} ${DC_WEB_DIR}/append.py ${footer} ${copy} PARENT_SCOPE)
endfunction()

# What the page loads. The game runs on a pthread; the main thread keeps the page responsive.
set(DC_WEB_LINK_OPTIONS
    -sPROXY_TO_PTHREAD=1
    -sASYNCIFY=1
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=268435456
    -sSTACK_SIZE=4194304
    -sDEFAULT_PTHREAD_STACK_SIZE=4194304
    -sFORCE_FILESYSTEM=1
    -sEXIT_RUNTIME=1
    -sENVIRONMENT=web,worker,node
    -sEXPORTED_RUNTIME_METHODS=FS,callMain
    -Wl,--error-limit=0)
