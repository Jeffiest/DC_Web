# The animation editor's model converter (convert.cpp), a build of its own that web.cmake runs as an external project
# with the same Emscripten toolchain. The game's build gives every unit -pthread and Emscripten's JavaScript
# exceptions; Kazarov's Rust core, built for wasm32-unknown-emscripten, has no atomics in its standard library and
# throws WebAssembly's own exceptions, so this links without threads and with -fwasm-exceptions. Kazarov's pools
# are the calling thread here (core.rs, thread_pool).
cmake_minimum_required(VERSION 3.20)
project(dc_anim_convert LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-fwasm-exceptions)
add_link_options(-fwasm-exceptions)

get_filename_component(DC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
include(${DC_ROOT}/tools/kazarov/cmake/KazarovRust.cmake)

add_executable(dc_anim_convert convert.cpp)
target_include_directories(dc_anim_convert PRIVATE ${DC_ROOT}/tools/dcdata)
target_link_libraries(dc_anim_convert PRIVATE kazarov_bridge kazarov_core)
target_link_options(dc_anim_convert PRIVATE
    -sMODULARIZE=1
    -sEXPORT_NAME=DcConvert
    -sINVOKE_RUN=0
    -sEXIT_RUNTIME=0
    -sALLOW_MEMORY_GROWTH=1
    -sMAXIMUM_MEMORY=4GB
    -sSTACK_SIZE=8388608
    -sFORCE_FILESYSTEM=1
    -sENVIRONMENT=worker,node
    -sEXPORTED_RUNTIME_METHODS=FS,callMain)
