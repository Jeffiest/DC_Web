# darkcloud_tests links the whole port and game so a test can exercise a
# unit against the real game types. Each *_test.cpp holds
# GoogleTest cases; ctest runs one process per case, since the game's state is
# global.
include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.18.0
    GIT_SHALLOW TRUE
    FIND_PACKAGE_ARGS NAMES GTest)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
add_executable(darkcloud_tests ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp ${TEST_SOURCES})
add_library(moonmod_native_plugin_fixture OBJECT moonmod_native_plugin_fixture.c)
set_target_properties(moonmod_native_plugin_fixture PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
target_include_directories(moonmod_native_plugin_fixture PRIVATE ${CMAKE_SOURCE_DIR}/port/include)
target_sources(darkcloud_tests PRIVATE $<TARGET_OBJECTS:moonmod_native_plugin_fixture>)
add_library(moonmod_native_plugin_shared_fixture SHARED moonmod_native_plugin_shared_fixture.c)
set_target_properties(moonmod_native_plugin_shared_fixture PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_include_directories(moonmod_native_plugin_shared_fixture PRIVATE ${CMAKE_SOURCE_DIR}/port/include)
target_compile_definitions(darkcloud_tests PRIVATE
    MOONMOD_TEST_NATIVE_PLUGIN_PATH="$<TARGET_FILE:moonmod_native_plugin_shared_fixture>"
    MOONMOD_TEST_GAME_ID="chronicle"
    MOONMOD_TEST_CHRONICLE=1)
set(MOONFACTORY_FIRST_STEPS_PATH "${CMAKE_SOURCE_DIR}/../MoonFactory/examples/first-steps")
if(EXISTS "${MOONFACTORY_FIRST_STEPS_PATH}/mod.json")
    file(TO_CMAKE_PATH "${MOONFACTORY_FIRST_STEPS_PATH}" MOONFACTORY_FIRST_STEPS_PATH)
    target_compile_definitions(darkcloud_tests PRIVATE
        MOONFACTORY_FIRST_STEPS_PATH="${MOONFACTORY_FIRST_STEPS_PATH}")
endif()
set(MOONFACTORY_ROGUELIKE_PATH "${CMAKE_SOURCE_DIR}/../MoonFactory/mods/moonmod.roguelike")
if(EXISTS "${MOONFACTORY_ROGUELIKE_PATH}/mod.json")
    file(TO_CMAKE_PATH "${MOONFACTORY_ROGUELIKE_PATH}" MOONFACTORY_ROGUELIKE_PATH)
    target_compile_definitions(darkcloud_tests PRIVATE
        MOONFACTORY_ROGUELIKE_PATH="${MOONFACTORY_ROGUELIKE_PATH}")
endif()
add_dependencies(darkcloud_tests moonmod_native_plugin_shared_fixture)
set_target_properties(darkcloud_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_include_directories(darkcloud_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tools/dcdata)
dc_link_game(darkcloud_tests)
kz_port_tests(darkcloud_tests)
target_link_libraries(darkcloud_tests PRIVATE GTest::gtest nlohmann_json::nlohmann_json ${CMAKE_DL_LIBS})

# A test that stalls is cut off; graphics startup can take several seconds on some systems. The clock
# cases measure short sleeps, which the parallel gfx cases skew, so they run on their own.
# Windows gets 60 by default, as Linux's CI sets: DungeonReach's Gauntlet floors took 10 to 28 seconds on the Windows
# runner, and went past 30 when it ran more tests beside them.
if(WIN32)
    set(DC_TEST_TIMEOUT_DEFAULT 60)
else()
    set(DC_TEST_TIMEOUT_DEFAULT 30)
endif()
set(DC_TEST_TIMEOUT ${DC_TEST_TIMEOUT_DEFAULT} CACHE STRING "Seconds a test case may run before it is cut off")
# The cases that need a Vulkan device have the gpu label, so a machine without one runs ctest -LE gpu. They are
# Kazarov's suites named GfxGpu* (its src/cpp/gfx/README.md, "Shared device test classification") and the ones
# that start a device under another name (GfxMisc, GfxTim2 and GfxCachedTarget), and every suite in a Chronicle
# test file that includes gfx_fixture.hpp or tex_fixture.hpp, found here so that a new one is labeled without a
# list to keep. Some start several renderers, each compiling every pipeline (seconds apiece, more on a software
# driver such as lavapipe), so they have their own limit.
set(DC_GPU_TEST_TIMEOUT 120 CACHE STRING "Seconds a test case that needs a Vulkan device may run")
set(device_suites GfxMisc GfxTim2 GfxCachedTarget)
foreach(source IN LISTS TEST_SOURCES)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${source})
    file(STRINGS ${source} fixture REGEX "^#include \"(gfx|tex)_fixture\\.hpp\"")
    if(fixture)
        file(STRINGS ${source} cases REGEX "^TEST(_F|_P)?\\([A-Za-z0-9_]+,")
        list(TRANSFORM cases REPLACE "^TEST(_F|_P)?\\(([A-Za-z0-9_]+),.*" "\\2")
        list(APPEND device_suites ${cases})
    endif()
endforeach()
list(REMOVE_DUPLICATES device_suites)
list(TRANSFORM device_suites APPEND ".*")
list(JOIN device_suites ":" device_filter)
set(device_filter "*GfxGpu*.*:${device_filter}")
gtest_discover_tests(darkcloud_tests
    DISCOVERY_MODE PRE_TEST
    TEST_FILTER -PlatformClock.*:${device_filter}
    PROPERTIES TIMEOUT ${DC_TEST_TIMEOUT} ENVIRONMENT "DC_TEST_FONT=${CMAKE_SOURCE_DIR}/tools/font/ChronicleSharp-Bold.ttf")
gtest_discover_tests(darkcloud_tests
    DISCOVERY_MODE PRE_TEST
    TEST_FILTER ${device_filter}
    PROPERTIES TIMEOUT ${DC_GPU_TEST_TIMEOUT} LABELS gpu
        ENVIRONMENT "DC_TEST_FONT=${CMAKE_SOURCE_DIR}/tools/font/ChronicleSharp-Bold.ttf")
gtest_discover_tests(darkcloud_tests
    DISCOVERY_MODE PRE_TEST
    TEST_FILTER PlatformClock.*
    PROPERTIES TIMEOUT ${DC_TEST_TIMEOUT} ENVIRONMENT "DC_TEST_FONT=${CMAKE_SOURCE_DIR}/tools/font/ChronicleSharp-Bold.ttf" RUN_SERIAL TRUE)

add_test(NAME Smoke.MoonModHeadlessPreflight
    COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:darkcloud>
        -DROOT=${CMAKE_BINARY_DIR}/smoke-preflight-fixture
        -P ${CMAKE_CURRENT_SOURCE_DIR}/smoke_headless_preflight.cmake)
set_tests_properties(Smoke.MoonModHeadlessPreflight PROPERTIES
    PASS_REGULAR_EXPRESSION "MOONFACTORY_PREFLIGHT_OK"
    TIMEOUT 120 RUN_SERIAL TRUE LABELS gpu)
