cmake_minimum_required(VERSION 3.24) # need multiconfig support
project(lodestone LANGUAGES CXX VERSION 0.1.0)

if (PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# OFF gives a client-only build: the manifest reader, the query layer, and manifest_dump. That build needs
# no Vulkan SDK, no Slang, and no Tint, so it configures under Emscripten. The default follows the
# platform: a desktop parent gets the cooker for its cook build step, and a web parent gets none.
if (EMSCRIPTEN)
    set(LODESTONE_BUILD_COOKER_DEFAULT OFF)
else()
    set(LODESTONE_BUILD_COOKER_DEFAULT ON)
endif()
option(LODESTONE_BUILD_COOKER "Build the cooker (Slang, Tint, and the lodestone library)" ${LODESTONE_BUILD_COOKER_DEFAULT})
# Every test links the cooker: the client tests build their bundles through the emitter. So the tests
# follow the cooker, and a client-only build has none.
if (LODESTONE_BUILD_COOKER)
    set(LODESTONE_BUILD_TESTS_DEFAULT ${PROJECT_IS_TOP_LEVEL})
else()
    set(LODESTONE_BUILD_TESTS_DEFAULT OFF)
endif()
# Lodestone tests don't use google test, but do use ctest
option(LODESTONE_BUILD_TESTS "Build the standalone unit test executables" ${LODESTONE_BUILD_TESTS_DEFAULT})
# These tools are standalone executables mostly intended for debugging or iterative shader work
option(LODESTONE_BUILD_TOOLS "Build the standalone tools" ${PROJECT_IS_TOP_LEVEL})
if (LODESTONE_BUILD_TESTS AND NOT LODESTONE_BUILD_COOKER)
    message(FATAL_ERROR "LODESTONE_BUILD_TESTS needs LODESTONE_BUILD_COOKER: every test links the cooker.")
endif()

# Add our various internal libraries first: the client library, and our
# JSON writer that is used by some of the .exe tools and the cooker lib
# these are all thin and worth using subdir add with
add_subdirectory(tools/json)
add_subdirectory(tools/suggest)
add_subdirectory(client)

if (LODESTONE_BUILD_COOKER)
    include(cmake/LodestoneCooker.cmake)
endif()

if(LODESTONE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# manifest_dump links only the client half. That link line is the proof that the client stays free of
# the compiler, so it builds in a client-only build too.
if(LODESTONE_BUILD_TOOLS)
    add_subdirectory(tools/manifest_dump)
    if (LODESTONE_BUILD_COOKER)
        add_subdirectory(tools/cooker_console)
    endif()
endif()
