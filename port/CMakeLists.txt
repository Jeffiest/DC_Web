# The x64 port: the game's code built with clang as C++26 for the host.
#
# port/src/game and port/include/game are the port's own copy of the game's
# code, forked from ps2/src and ps2/include; the rest of port/src is the
# platform layer it runs on. ps2/ is the matching decompilation alone and the
# port does not compile it.

if(NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER clang++)
endif()
enable_language(CXX)
if(NOT CMAKE_C_COMPILER AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    get_filename_component(moonmod_cxx_dir ${CMAKE_CXX_COMPILER} DIRECTORY)
    get_filename_component(moonmod_cxx_name ${CMAKE_CXX_COMPILER} NAME)
    string(REGEX REPLACE "clang\\+\\+([.]exe)?$" "clang\\1" moonmod_c_name ${moonmod_cxx_name})
    if(NOT moonmod_c_name STREQUAL moonmod_cxx_name)
        set(moonmod_sibling_c "${moonmod_cxx_dir}/${moonmod_c_name}")
        if(EXISTS ${moonmod_sibling_c})
            set(CMAKE_C_COMPILER ${moonmod_sibling_c} CACHE FILEPATH "C compiler paired with clang++")
        endif()
    endif()
endif()
enable_language(C)

# MoonMod's files are made for one Chronicle commit; pasted over another they may not build.
set(MOONMOD_CHRONICLE_COMMIT 9418a5506e2a08e7739aa68d70841df6bc516201)
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE moonmod_chronicle_head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
        RESULT_VARIABLE moonmod_git_result)
    if(moonmod_git_result EQUAL 0 AND NOT moonmod_chronicle_head STREQUAL MOONMOD_CHRONICLE_COMMIT)
        message(WARNING "MoonMod is made for Chronicle ${MOONMOD_CHRONICLE_COMMIT}, but this checkout is at "
            "${moonmod_chronicle_head}. Run 'git checkout ${MOONMOD_CHRONICLE_COMMIT}' and paste the MoonMod files "
            "again, or use the MoonMod release made for this commit.")
    endif()
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NINTENDO_SWITCH)
    # newlib exposes some POSIX declarations only outside strict ISO mode.
    set(CMAKE_CXX_EXTENSIONS ON)
else()
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# darkcloud is position-independent (DC_LINK_OPTIONS below).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(EMSCRIPTEN)
    # WebAssembly has one statically linked module; position-independent code would ask for dynamic linking.
    set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
endif()

set(KZ_ROOT "${KAZAROV_SOURCE_DIR}")
if(NOT EXISTS ${KZ_ROOT}/include/kazarov/port.h)
    message(FATAL_ERROR "${KZ_ROOT} is empty: run git submodule update --init tools/kazarov")
endif()

# Archives Kazarov's FetchContent_Declare downloads take their extraction time, so a changed URL
# rebuilds what they hold. Set before the include: its functions keep the policies they were defined under.
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
include(${KZ_ROOT}/cmake/KazarovPort.cmake)

if(NINTENDO_SWITCH)
    include(${CMAKE_CURRENT_SOURCE_DIR}/switch/switch.cmake)
endif()

# Python runs the port override script from the Kazarov submodule.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
if(EMSCRIPTEN)
    include(${CMAKE_CURRENT_SOURCE_DIR}/web/web.cmake)
endif()
# MoonMod's small embedded Lua runtime keeps the PC build self-contained and offline-capable.
set(MOONMOD_LUA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua-5.5.1)
file(GLOB MOONMOD_LUA_SOURCES CONFIGURE_DEPENDS ${MOONMOD_LUA_DIR}/*.c)
list(FILTER MOONMOD_LUA_SOURCES EXCLUDE REGEX "/(lua|luac|ltests)\.c$")
add_library(moonmod_lua STATIC ${MOONMOD_LUA_SOURCES})
target_include_directories(moonmod_lua PUBLIC ${MOONMOD_LUA_DIR})
set_target_properties(moonmod_lua PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
if(UNIX AND NOT APPLE)
    target_link_libraries(moonmod_lua PUBLIC m)
endif()

# Flatpak builds have no network: their single-file libraries are staged in audio_decoders/ and stb_truetype/; others fetch them.
set(KZ_PORT_VENDOR_DIRS ${CMAKE_SOURCE_DIR}/audio_decoders ${CMAKE_SOURCE_DIR}/stb_truetype)
# The Flatpak also stages each library Kazarov's FetchContent_Declare downloads, in flatpak-deps/<name>
# (scripts/port/flatpak_sources.py checks the manifest), and FetchContent takes it from there. Other
# builds have no flatpak-deps/ and download them as declared.
file(GLOB staged_dependencies LIST_DIRECTORIES true "${CMAKE_SOURCE_DIR}/flatpak-deps/*")
foreach(staged IN LISTS staged_dependencies)
    if(IS_DIRECTORY "${staged}")
        get_filename_component(dependency "${staged}" NAME)
        string(TOUPPER "${dependency}" dependency_upper)
        set(FETCHCONTENT_SOURCE_DIR_${dependency_upper} "${staged}")
    endif()
endforeach()
kz_port_host_library(dc_host)
if(NINTENDO_SWITCH)
    dc_switch_host(dc_host)
elseif(EMSCRIPTEN)
    dc_web_host(dc_host)
    dc_web_vulkan(dc_host)
endif()
# Chronicle still loads retail HD/BD/SQ audio. These compatibility declarations extend the shared
# semantic audio API; every host and game unit must see the same class definitions.
target_include_directories(dc_host BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/port/include)

set(GAME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/game)
set(PORT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS ${GAME_DIR}/*.cpp)
file(GLOB_RECURSE PORT_SOURCES CONFIGURE_DEPENDS ${PORT_DIR}/*.cpp)
# The game's own units are dc_ps2's. Removed by name, not by a regex built from the path: a checkout under a
# folder with regex characters in its name (Program Files (x86)) would keep them, and every one would be linked twice.
list(REMOVE_ITEM PORT_SOURCES ${GAME_SOURCES})
list(FILTER PORT_SOURCES EXCLUDE REGEX "/tests/")
if(NINTENDO_SWITCH OR EMSCRIPTEN)
    list(FILTER PORT_SOURCES EXCLUDE REGEX "/platform/(discord|firstrun)\.cpp$")
endif()
set(PORT_DEFINITIONS PORT PAL)

# What both halves are compiled with. The game was written for MWCC, which
# accepts constructs clang rejects by default: pointers truncated to 32-bit
# integers, narrowing in braced initialisers and the register keyword.
add_library(dc_options INTERFACE)
target_compile_definitions(dc_options INTERFACE ${PORT_DEFINITIONS})
target_include_directories(dc_options INTERFACE
    ${CMAKE_SOURCE_DIR}/port/include ${KZ_ROOT}/include ${CMAKE_SOURCE_DIR}/port/include/game ${CMAKE_SOURCE_DIR}/port/include/game/sce)
target_compile_options(dc_options INTERFACE
    "SHELL:-include \"${CMAKE_SOURCE_DIR}/port/include/port.h\""
    -fms-extensions -ffunction-sections -fdata-sections -Wno-register)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(dc_options INTERFACE -fpermissive -Wno-narrowing -fno-aggressive-loop-optimizations)
else()
    target_compile_options(dc_options INTERFACE -Wno-c++11-narrowing -Wno-return-mismatch)
endif()
# arm64 clang fuses a*b+c into one rounding by default; x86-64 has no FMA in its
# baseline. Off everywhere so the game's arithmetic rounds the same on both.
target_compile_options(dc_options INTERFACE -ffp-contract=off)
if(ANDROID)
    # PS2's s8 is plain char; arm64 clang otherwise treats its table sentinels as unsigned.
    target_compile_options(dc_options INTERFACE -fsigned-char)
endif()

if(WIN32 AND (NOT MINGW OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang"))
    message(FATAL_ERROR "The Windows build requires clang/llvm-mingw")
endif()

# The game units may include the platform layer's headers (port/src and the generated headers), so
# dc_ps2 gets dc_port's include path and libraries; dc_port_includes below adds the include path.
add_library(dc_ps2 OBJECT ${GAME_SOURCES})
target_link_libraries(dc_ps2 PRIVATE dc_options SDL3::SDL3 Vulkan::Vulkan nlohmann_json::nlohmann_json kz_port_headers)
# Retail's code reads locals it never set and falls off the end of functions
# that return a value, and ps2/src keeps both because the match depends on
# them. MWCC gave each a defined if arbitrary result; these flags give the port
# a defined one too, along with the type punning and wraparound the code assumes.
target_compile_options(dc_ps2 PRIVATE -ftrivial-auto-var-init=zero -fno-strict-aliasing -fwrapv)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(dc_ps2 PRIVATE -Werror=return-type)
else()
    target_compile_options(dc_ps2 PRIVATE -fno-strict-return)
endif()
# What is left on warns about behaviour the port may still need to answer for:
# uninitialised reads, missing returns, format strings, overlapping copies and
# the like. What is turned off describes how MWCC-era source is spelled -- string
# literals as char *, MWCC's pragmas, 32-bit pointer casts, the unused names
# and expressions matching leaves behind -- which ps2/src cannot change without
# changing what MWCC compiles.
target_compile_options(dc_ps2 PRIVATE -Wall -Wno-writable-strings -Wno-unknown-pragmas
    -Wno-mismatched-tags -Wno-missing-braces -Wno-char-subscripts -Wno-overloaded-virtual
    -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-int-to-void-pointer-cast
    -Wno-void-pointer-to-int-cast -Wno-unused-variable -Wno-unused-but-set-variable
    -Wno-unused-local-typedef -Wno-unused-label -Wno-unused-function -Wno-unused-value
    -Wno-unused-const-variable -Wno-undefined-internal -Wno-self-assign -Wno-deprecated-volatile)
if(ANDROID)
    target_compile_options(dc_ps2 PRIVATE -Wno-format-security -Wno-return-type)
endif()

set(PORT_LIB_SOURCES ${PORT_SOURCES})
list(FILTER PORT_LIB_SOURCES EXCLUDE REGEX "/tests/|/main\\.cpp$")

set(SHADER_HEADER_DIR ${CMAKE_BINARY_DIR}/generated)


# port/icon/chronicle-128.rgba, the window icon's raw 128x128 RGBA pixels, as a byte array.
set(ICON_RGBA ${CMAKE_SOURCE_DIR}/port/icon/chronicle-128.rgba)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ICON_RGBA})
file(READ ${ICON_RGBA} icon_hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," icon_bytes "${icon_hex}")
file(CONFIGURE OUTPUT ${SHADER_HEADER_DIR}/icon/chronicle.hpp
    CONTENT "#pragma once\n#include <cstdint>\ninline constexpr int kIconSize = 128;\ninline constexpr std::uint8_t kIconRgba[] = {${icon_bytes}};\n")

# CREDITS, one contributor a line and # for comments, as the About page's list of names.
set(CREDITS_FILE ${CMAKE_SOURCE_DIR}/CREDITS)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CREDITS_FILE})
file(STRINGS ${CREDITS_FILE} credit_lines ENCODING UTF-8)
set(credit_names "")
foreach(line IN LISTS credit_lines)
    string(STRIP "${line}" line)
    if(line STREQUAL "" OR line MATCHES "^#")
        continue()
    endif()
    string(REPLACE "\\" "\\\\" line "${line}")
    string(REPLACE "\"" "\\\"" line "${line}")
    string(APPEND credit_names "\"${line}\", ")
endforeach()
if(credit_names STREQUAL "")
    set(credit_names "\"\"")
endif()
file(CONFIGURE OUTPUT ${SHADER_HEADER_DIR}/options/credits.hpp
    CONTENT "#pragma once\ninline constexpr const char *kCredits[] = {${credit_names}};\n")

kz_port_sce_sources(DC_SCE_SOURCES LIBS libvu0 libpad libmc sifdev sifrpc eekernel libcdvd libgraph libdma libpkt)
add_library(dc_port OBJECT ${PORT_LIB_SOURCES} ${DC_SCE_SOURCES})
if(NINTENDO_SWITCH)
    dc_switch_port(dc_port)
elseif(EMSCRIPTEN)
    dc_web_port(dc_port)
endif()
target_link_libraries(dc_port PRIVATE dc_options SDL3::SDL3 Vulkan::Vulkan nlohmann_json::nlohmann_json fastgltf::fastgltf kz_port_headers)
target_link_libraries(dc_port PRIVATE moonmod_lua ${CMAKE_DL_LIBS})
# Online play's socket (moonmod/net_session.hpp, coop/relay): the browser's WebSocket on the web (web.cmake links
# Emscripten's), WinHTTP on Windows, libcurl's TLS connection on Linux and macOS where it is found. Without one,
# net.* reports itself unsupported. DC_COOP_RELAY_DEFAULT is the relay a mod's net.host/join use unless it names one.
# The public relay coop-relay.yml deploys (coop/relay/README.md); a build for a private server sets its own.
set(DC_COOP_RELAY_DEFAULT "wss://chronicle-coop-relay.jeffery-s-rose.workers.dev" CACHE STRING "The co-op relay (wss://...) online play uses by default")
target_compile_definitions(dc_port PRIVATE DC_COOP_RELAY_DEFAULT="${DC_COOP_RELAY_DEFAULT}")
set(DC_NET_LIBS)
if(WIN32)
    set(DC_NET_LIBS winhttp)
elseif(APPLE)
    # The SDK's libcurl, linked by name: CURL::libcurl's include directory is the SDK's usr/include, which put ahead
    # of libc++'s own headers breaks them.
    target_compile_definitions(dc_port PRIVATE DC_HAVE_CURL)
    set(DC_NET_LIBS curl)
elseif(NOT EMSCRIPTEN AND NOT NINTENDO_SWITCH AND NOT ANDROID)
    find_package(CURL QUIET)
    if(CURL_FOUND)
        target_compile_definitions(dc_port PRIVATE DC_HAVE_CURL)
        target_link_libraries(dc_port PRIVATE CURL::libcurl)
        set(DC_NET_LIBS CURL::libcurl)
    else()
        message(STATUS "libcurl not found: this build has no online play")
    endif()
endif()
# port/src, the generated headers, dcdata's header-only core and the stb directories: captionfont.cpp
# rasterizes with stb_truetype and platform/png_image.cpp decodes with stb_image, which Kazarov stages
# (or fetches) for dc_host.
function(dc_port_includes target)
    target_include_directories(${target} PRIVATE ${PORT_DIR} ${SHADER_HEADER_DIR} ${CMAKE_SOURCE_DIR}/tools/dcdata)
    get_target_property(dc_host_includes dc_host INCLUDE_DIRECTORIES)
    foreach(include_dir IN LISTS dc_host_includes)
        if(EXISTS ${include_dir}/stb_truetype.h OR EXISTS ${include_dir}/stb_image.h)
            target_include_directories(${target} PRIVATE ${include_dir})
        endif()
    endforeach()
endfunction()
dc_port_includes(dc_port)
dc_port_includes(dc_ps2)
target_include_directories(dc_port PRIVATE ${MOONMOD_LUA_DIR})
# The game's headers trip a few warnings that only describe how MWCC's source
# was written, not a problem in the port.
target_compile_options(dc_port PRIVATE -Wall -Wno-mismatched-tags -Wno-unused-label
    -Wno-overloaded-virtual -Wno-unused-function)

# Shared by the game and its tests: the port's objects, the game's and the link
# flags they need. The executable is position-independent on every platform:
# the game's casts of pointers to 32-bit integers are all widened in port/src
# (docs/port/truncations.md), so nothing has to sit below 4 GiB.
# Names the PS2 build gives through ps2/config/*/object_fixups.json are
# ordinary definitions in the game units (EditGaijiTbl in
# port/src/game/clsmes.cpp). GetItem in port/src/game/dngstatusdata.cpp reads WeaponList directly rather than
# through ItemPutListTbl12_bytes.
if(EMSCRIPTEN)
    set(DC_LINK_OPTIONS ${DC_WEB_LINK_OPTIONS})
elseif(NINTENDO_SWITCH)
    set(DC_LINK_OPTIONS -Wl,--gc-sections)
elseif(WIN32)
    set(DC_LINK_OPTIONS -fuse-ld=lld -Wl,--gc-sections -Wl,--error-limit=0)
elseif(APPLE)
    set(DC_LINK_OPTIONS -Wl,-dead_strip)
else()
    set(DC_LINK_OPTIONS -fuse-ld=lld -Wl,--gc-sections -Wl,--error-limit=0)
    if(ANDROID)
        list(APPEND DC_LINK_OPTIONS -Wl,-z,max-page-size=16384)
    else()
        list(APPEND DC_LINK_OPTIONS -pie -Wl,-z,separate-code)
    endif()
endif()

function(dc_link_game target)
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:dc_port> $<TARGET_OBJECTS:dc_ps2>)
    target_link_libraries(${target} PRIVATE dc_options dc_host kz_port_headers SDL3::SDL3 Vulkan::Vulkan fastgltf::fastgltf
        nlohmann_json::nlohmann_json moonmod_lua ${DC_NET_LIBS} ${CMAKE_DL_LIBS})
    target_include_directories(${target} PRIVATE ${PORT_DIR} ${CMAKE_SOURCE_DIR}/tools/dcdata)
    target_compile_options(${target} PRIVATE -Wall -Wno-mismatched-tags -Wno-unused-label
        -Wno-overloaded-virtual -Wno-unused-function)
    target_link_options(${target} PRIVATE ${DC_LINK_OPTIONS})
endfunction()

if(ANDROID)
    add_library(main SHARED ${PORT_DIR}/main.cpp)
    target_link_libraries(main PRIVATE log)
    set_target_properties(main PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    set(darkcloud_target main)
else()
    add_executable(darkcloud ${PORT_DIR}/main.cpp)
    set_target_properties(darkcloud PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    set(darkcloud_target darkcloud)
endif()
dc_link_game(${darkcloud_target})
if(NINTENDO_SWITCH)
    dc_switch_nro(darkcloud)
endif()
if(EMSCRIPTEN)
    # The WebGPU build, and the same game on WebGL2 (web/web.cmake).
    dc_web_link_vulkan(darkcloud webgpu)
    add_executable(darkcloud_gl ${PORT_DIR}/main.cpp)
    set_target_properties(darkcloud_gl PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    dc_link_game(darkcloud_gl)
    dc_web_link_vulkan(darkcloud_gl gl)
    add_dependencies(darkcloud dc_web_site dc_web_builtin_mods)
    # After darkcloud, which has lang/ and glyphs/ copied for both to preload.
    add_dependencies(darkcloud_gl darkcloud)
    dc_web_gfx_tests()
endif()
if(WIN32)
    target_link_options(darkcloud PRIVATE -municode)
    enable_language(RC)
    target_sources(darkcloud PRIVATE ${CMAKE_SOURCE_DIR}/port/icon/chronicle.rc)
endif()

include(GNUInstallDirs)
if(NOT ANDROID AND NOT NINTENDO_SWITCH)
    install(TARGETS darkcloud RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# The strings the port draws itself, translated (docs/LOCALIZATION.md): lang/ beside the executable, where the
# game reads its language files.
if(ANDROID)
    set(ANDROID_ASSETS ${CMAKE_SOURCE_DIR}/port/android/app/src/main/assets)
    file(MAKE_DIRECTORY ${ANDROID_ASSETS}/lang ${ANDROID_ASSETS}/glyphs)
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/lang/ DESTINATION ${ANDROID_ASSETS}/lang)
    configure_file(${CMAKE_SOURCE_DIR}/tools/font/ChronicleSharp-Bold.ttf ${ANDROID_ASSETS}/lang/font.ttf COPYONLY)
    configure_file(${CMAKE_SOURCE_DIR}/tools/font/ChronicleSharp-Regular.ttf ${ANDROID_ASSETS}/lang/font_thin.ttf COPYONLY)
else()
    add_custom_target(dc_lang
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/lang $<TARGET_FILE_DIR:darkcloud>/lang
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/tools/font/ChronicleSharp-Bold.ttf
                $<TARGET_FILE_DIR:darkcloud>/lang/font.ttf
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/tools/font/ChronicleSharp-Regular.ttf
                $<TARGET_FILE_DIR:darkcloud>/lang/font_thin.ttf
        COMMENT "Copying the port's translated strings and text fonts")
    add_dependencies(${darkcloud_target} dc_lang)
    install(DIRECTORY lang/ DESTINATION ${CMAKE_INSTALL_BINDIR}/lang)
    install(FILES ${CMAKE_SOURCE_DIR}/tools/font/ChronicleSharp-Bold.ttf DESTINATION ${CMAKE_INSTALL_BINDIR}/lang RENAME font.ttf)
    install(FILES ${CMAKE_SOURCE_DIR}/tools/font/ChronicleSharp-Regular.ttf DESTINATION ${CMAKE_INSTALL_BINDIR}/lang RENAME font_thin.ttf)
endif()

# The button symbols (docs/GLYPHS.md): glyphs/ beside the executable. Rebuilt from art by tools/glyphs/build_glyphs.py.
if(ANDROID)
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/glyphs/ DESTINATION ${ANDROID_ASSETS}/glyphs)
else()
    add_custom_target(dc_glyphs
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/glyphs $<TARGET_FILE_DIR:darkcloud>/glyphs
        COMMENT "Copying the button symbols")
    add_dependencies(${darkcloud_target} dc_glyphs)
    install(DIRECTORY glyphs/ DESTINATION ${CMAKE_INSTALL_BINDIR}/glyphs)
endif()

# The tests use an installed GoogleTest, or fetch one at configure time; a build with neither turns them off.
option(DC_BUILD_TESTS "Build darkcloud_tests" ON)
if(DC_BUILD_TESTS AND NOT ANDROID AND NOT NINTENDO_SWITCH AND NOT EMSCRIPTEN)
    enable_testing()
    add_subdirectory(src/tests)
endif()
if(NOT ANDROID AND NOT NINTENDO_SWITCH AND NOT EMSCRIPTEN)
    add_subdirectory(${CMAKE_SOURCE_DIR}/tools/dcdata ${CMAKE_BINARY_DIR}/tools/dcdata)
endif()
