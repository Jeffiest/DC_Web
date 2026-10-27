# The browser build (PLATFORM WEB), included by port/CMakeLists.txt with Emscripten's toolchain.
#
# The game and Kazarov's host code build to WebAssembly unchanged where they can:
# - SDL3 is built from source for Emscripten.
# - Browsers have no Vulkan. The renderer compiles against Vulkan's headers, and vulkan/ implements the
#   commands it uses on WebGPU (Emscripten's emdawnwebgpu port), gl/ on WebGL2; weak stubs that fail stand for
#   the rest (gen_vk_stubs.py).
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
# patch_sdl.py fixes the dummy audio driver's timer, which headless runs use.
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.18
    GIT_SHALLOW TRUE
    PATCH_COMMAND ${Python3_EXECUTABLE} ${DC_WEB_DIR}/patch_sdl.py <SOURCE_DIR>
    UPDATE_DISCONNECTED TRUE)
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

# The renderer's shaders in WGSL, for Vulkan on WebGPU.
set(DC_WGSL ${CMAKE_BINARY_DIR}/web/wgsl.cpp)
file(GLOB DC_WGSL_SOURCES CONFIGURE_DEPENDS ${DC_WEB_DIR}/vulkan/shaders/*.wgsl)
add_custom_command(OUTPUT ${DC_WGSL}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/web
    COMMAND ${Python3_EXECUTABLE} ${DC_WEB_DIR}/vulkan/embed_wgsl.py ${DC_WEB_DIR}/vulkan/shaders ${DC_WGSL}
    DEPENDS ${DC_WEB_DIR}/vulkan/embed_wgsl.py ${DC_WGSL_SOURCES}
    COMMENT "Embedding the WGSL shaders"
    VERBATIM)

# Kazarov's host library with the arena memory replaced (Emscripten's mmap has no reservations or guard
# pages).
function(dc_web_host target)
    set_source_files_properties(${KZ_PORT_ROOT}/platform/memory.cpp PROPERTIES HEADER_FILE_ONLY ON)
    target_sources(${target} PRIVATE ${DC_WEB_DIR}/src/memory.cpp)
endfunction()

# The two Vulkans the renderer is linked with, each an object library so the stubs' weak definitions never
# stand in for its commands: Vulkan on WebGPU (vulkan/, Emscripten's emdawnwebgpu port) for darkcloud, and
# Vulkan on WebGL2 (gl/) for darkcloud_gl, which the page loads where WebGPU draws the game wrong or slowly
# (docs/WEB.md). Both match the renderer's SPIR-V, compiled for dc_host.
function(dc_web_vulkan host)
    add_library(dc_vk_webgpu OBJECT
        ${DC_WEB_DIR}/vulkan/device.cpp ${DC_WEB_DIR}/vulkan/objects.cpp ${DC_WEB_DIR}/vulkan/commands.cpp ${DC_WGSL})
    add_library(dc_vk_gl OBJECT
        ${DC_WEB_DIR}/gl/device.cpp ${DC_WEB_DIR}/gl/objects.cpp ${DC_WEB_DIR}/gl/commands.cpp
        ${DC_WEB_DIR}/gl/shaders.cpp)
    target_include_directories(dc_vk_webgpu PRIVATE ${DC_WEB_DIR}/vulkan ${CMAKE_BINARY_DIR}/kz_port/generated)
    target_include_directories(dc_vk_gl PRIVATE ${DC_WEB_DIR}/gl ${CMAKE_BINARY_DIR}/kz_port/generated)
    target_compile_options(dc_vk_webgpu PRIVATE --use-port=emdawnwebgpu)
    foreach(layer dc_vk_webgpu dc_vk_gl)
        target_link_libraries(${layer} PRIVATE Vulkan::Headers)
        # The SPIR-V headers are made with the host library.
        add_dependencies(${layer} ${host})
    endforeach()
endfunction()

# What a target linking the WebGPU or the WebGL2 Vulkan takes: the layer's objects and its link options.
function(dc_web_link_vulkan target renderer)
    if(renderer STREQUAL "gl")
        target_sources(${target} PRIVATE $<TARGET_OBJECTS:dc_vk_gl>)
        target_link_options(${target} PRIVATE -sMAX_WEBGL_VERSION=2 -sMIN_WEBGL_VERSION=2)
    else()
        target_sources(${target} PRIVATE $<TARGET_OBJECTS:dc_vk_webgpu>)
        target_link_options(${target} PRIVATE --use-port=emdawnwebgpu)
    endif()
endfunction()

# The desktop-only units, replaced.
function(dc_web_port target)
    target_sources(${target} PRIVATE ${DC_WEB_DIR}/src/discord.cpp ${DC_WEB_DIR}/src/firstrun.cpp
        ${DC_WEB_DIR}/src/storage.cpp ${DC_WEB_DIR}/src/exit.cpp ${DC_WEB_DIR}/src/touch.cpp)
endfunction()

# The page around the game (site/): copied beside darkcloud.js, so the build directory serves as is.
file(GLOB DC_WEB_SITE CONFIGURE_DEPENDS ${DC_WEB_DIR}/site/*)
add_custom_target(dc_web_site
    COMMAND ${CMAKE_COMMAND} -E copy ${DC_WEB_SITE} ${CMAKE_BINARY_DIR}
    COMMENT "Copying the web page")

# What the page loads. The game runs on a pthread; the main thread keeps the page responsive, and the canvas
# is handed to the game's thread, where WebGPU or WebGL2 draws to it. WasmFS mounts the browser's storage
# (src/storage.cpp). lang/ and glyphs/, which the game reads beside its executable, are preloaded into the
# root: the executable is ./this.program there.
set(DC_WEB_LINK_OPTIONS
    -sOFFSCREENCANVAS_SUPPORT=1
    "-sOFFSCREENCANVASES_TO_PTHREAD=#canvas"
    -sWASMFS=1
    "SHELL:--preload-file ${CMAKE_BINARY_DIR}/lang@/lang"
    "SHELL:--preload-file ${CMAKE_BINARY_DIR}/glyphs@/glyphs"
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

# The WebGPU probe (probe/): what the renderer's WebGPU backend will rely on, with the game's thread model.
add_executable(webgpu_probe ${DC_WEB_DIR}/probe/webgpu_probe.cpp)
set_target_properties(webgpu_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_compile_options(webgpu_probe PRIVATE --use-port=emdawnwebgpu)
target_link_options(webgpu_probe PRIVATE
    --use-port=emdawnwebgpu
    -sPROXY_TO_PTHREAD=1
    -sASYNCIFY=1
    -sALLOW_MEMORY_GROWTH=1
    -sOFFSCREENCANVAS_SUPPORT=1
    "-sOFFSCREENCANVASES_TO_PTHREAD=#canvas"
    -sEXIT_RUNTIME=1
    -sENVIRONMENT=web,worker)
add_custom_command(TARGET webgpu_probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy ${DC_WEB_DIR}/probe/probe.html ${CMAKE_BINARY_DIR}
    VERBATIM)
add_dependencies(webgpu_probe dc_web_site)

# Kazarov's renderer tests against Vulkan on WebGPU and on WebGL2, for the browser (test/gfx_tests.html, whose
# ?renderer=gl loads the second). Not built by default: `cmake --build --preset web --target kazarov_gfx_tests
# kazarov_gfx_tests_gl`. The render thread and depth planes cases need what the web leaves out (a worker thread
# on the device, bindless textures).
function(dc_web_gfx_tests)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.18.0
        GIT_SHALLOW TRUE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    file(GLOB tests CONFIGURE_DEPENDS ${KZ_PORT_ROOT}/tests/gfx_*_test.cpp)
    list(FILTER tests EXCLUDE REGEX "gfx_(render_thread|depth_planes)_test\\.cpp$")
    foreach(renderer webgpu gl)
        set(target kazarov_gfx_tests)
        if(renderer STREQUAL "gl")
            set(target kazarov_gfx_tests_gl)
        endif()
        add_executable(${target} EXCLUDE_FROM_ALL ${DC_WEB_DIR}/test/gfx_tests_main.cpp ${tests})
        set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
        target_include_directories(${target} PRIVATE ${KZ_PORT_INCLUDE_ROOT}/tests ${KZ_PORT_INCLUDE_ROOT}
            ${CMAKE_BINARY_DIR}/kz_port/generated)
        target_link_libraries(${target} PRIVATE dc_host kz_port_headers SDL3::SDL3 Vulkan::Vulkan
            fastgltf::fastgltf nlohmann_json::nlohmann_json GTest::gtest)
        dc_web_link_vulkan(${target} ${renderer})
        if(renderer STREQUAL "gl")
            # WebGL2 draws offscreen into a context on the page's canvas.
            target_link_options(${target} PRIVATE -sOFFSCREENCANVAS_SUPPORT=1 "-sOFFSCREENCANVASES_TO_PTHREAD=#canvas")
        endif()
        target_link_options(${target} PRIVATE
            -sPROXY_TO_PTHREAD=1
            -sASYNCIFY=1
            -sALLOW_MEMORY_GROWTH=1
            -sINITIAL_MEMORY=268435456
            -sSTACK_SIZE=4194304
            -sDEFAULT_PTHREAD_STACK_SIZE=4194304
            -sWASMFS=1
            -sEXIT_RUNTIME=1
            -sENVIRONMENT=web,worker
            -Wl,--error-limit=0)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy ${DC_WEB_DIR}/test/gfx_tests.html ${CMAKE_BINARY_DIR}
            VERBATIM)
    endforeach()
endfunction()
