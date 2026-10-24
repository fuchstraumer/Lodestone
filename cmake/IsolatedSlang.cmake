# Slang and SPIRV-Tools, each in its own build tree (ExternalProject). LODESTONE_ISOLATE_SLANG selects this file, and
# cmake/ConfigureSlang.cmake (add_slang_subdirectory) is the in-tree fallback.
#
# Why a separate tree:
# - No flag, cache entry, or target of this project reaches the Slang build, so a change here cannot rebuild Slang. Its
#   build step runs Ninja in its own tree, which does nothing when nothing in Slang changed.
# - Slang always builds Release with LTO, for every configuration of this project. Its API crosses the DLL boundary as
#   COM-style interfaces with no C++ library type, so a Debug lodestone can load a Release slang-compiler.dll. Before
#   this, RelWithDebInfo compiled Slang with /Ob1, and Debug compiled a Debug Slang.
# - Slang's mimalloc stays inside slang-compiler.dll, which exports none of it. In-tree, SPIRV-Tools-static linked
#   mimalloc-static (v2), and every executable that linked lodestone linked that mimalloc too. A parent project with
#   its own mimalloc then had two copies of every mi_ symbol.
#
# SPIRV-Tools builds from the same submodule (third_party/slang/external/spirv-tools), static, with no mimalloc and no
# tools, and only the SPIRV-Tools-static target. It is C++ and shares this project's CRT, so it builds in the
# configuration of this project. A parent project that already defines SPIRV-Tools-static or SPIRV-Headers keeps its
# own target.
#
# Requires: cmake/ConfigureSlang.cmake (LODESTONE_SLANG_FORWARDED_OPTIONS and copy_slang_dlls_to_target), and
# Vulkan_INCLUDE_DIR from find_package(Vulkan).

include(ExternalProject)

set(LODESTONE_ISOLATED_DIR "${CMAKE_BINARY_DIR}/_isolated")
set(LODESTONE_SLANG_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/slang")

# --- Slang ---

set(slangBinaryDir "${LODESTONE_ISOLATED_DIR}/slang-build")
set(slangOutputDir "${slangBinaryDir}/Release")
set(slangLibrary "${slangOutputDir}/lib/slang-compiler.lib")
set(slangRuntime "${slangOutputDir}/bin/slang-compiler.dll")
# The source headers, and the folder where configure_file writes slang-tag-version.h. Not Release/include:
# copy_slang_headers copies the headers there at every build with a new time, and each file that includes slang.h
# then compiled again at every build.
set(slangIncludeDirs "${LODESTONE_SLANG_SOURCE_DIR}/include" "${slangBinaryDir}/source/slang/slang-version-header")

set(slangCacheArgs
    "-DCMAKE_BUILD_TYPE:STRING=Release"
    "-DCMAKE_C_COMPILER:STRING=${CMAKE_C_COMPILER}"
    "-DCMAKE_CXX_COMPILER:STRING=${CMAKE_CXX_COMPILER}"
    "-DCMAKE_MODULE_PATH:PATH=${CMAKE_CURRENT_SOURCE_DIR}/cmake"
    "-DVulkan_INCLUDE_DIR:PATH=${Vulkan_INCLUDE_DIR}"
    "-DVulkanHeaders_INCLUDE_DIR:PATH=${Vulkan_INCLUDE_DIR}")
foreach (option IN LISTS LODESTONE_SLANG_FORWARDED_OPTIONS)
    list(APPEND slangCacheArgs "-D${option}:BOOL=${${option}}")
endforeach()

ExternalProject_Add(lodestone_slang_build
    SOURCE_DIR "${LODESTONE_SLANG_SOURCE_DIR}"
    BINARY_DIR "${slangBinaryDir}"
    PREFIX "${LODESTONE_ISOLATED_DIR}/slang"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    CMAKE_GENERATOR Ninja
    CMAKE_CACHE_ARGS ${slangCacheArgs}
    BUILD_COMMAND "${CMAKE_COMMAND}" --build "${slangBinaryDir}" --target slang
    # the stamp would skip the build after the first one. Ninja in the Slang tree decides instead
    BUILD_ALWAYS ON
    BUILD_BYPRODUCTS "${slangLibrary}" "${slangRuntime}"
    INSTALL_COMMAND ""
    USES_TERMINAL_CONFIGURE ON
    USES_TERMINAL_BUILD ON
    EXCLUDE_FROM_ALL ON)

# the version folder exists only after the first configure, and an imported target cannot name a folder that does not
# exist
file(MAKE_DIRECTORY "${slangBinaryDir}/source/slang/slang-version-header")
add_library(slang SHARED IMPORTED GLOBAL)
set_target_properties(slang PROPERTIES
    IMPORTED_LOCATION "${slangRuntime}"
    IMPORTED_IMPLIB "${slangLibrary}"
    INTERFACE_INCLUDE_DIRECTORIES "${slangIncludeDirs}")
add_dependencies(slang lodestone_slang_build)

# --- SPIRV-Headers (header only) ---

if (NOT TARGET SPIRV-Headers)
    # Dawn adds its own SPIRV-Headers target unless one exists, so this one must come first
    add_library(SPIRV-Headers INTERFACE)
    target_include_directories(SPIRV-Headers INTERFACE "${LODESTONE_SLANG_SOURCE_DIR}/external/spirv-headers/include")
endif()
if (NOT TARGET SPIRV-Headers::SPIRV-Headers)
    add_library(SPIRV-Headers::SPIRV-Headers ALIAS SPIRV-Headers)
endif()

# --- SPIRV-Tools ---

if (NOT TARGET SPIRV-Tools-static)
    set(spirvToolsSourceDir "${LODESTONE_SLANG_SOURCE_DIR}/external/spirv-tools")
    set(spirvToolsBinaryDir "${LODESTONE_ISOLATED_DIR}/spirv-tools-build")
    # one configure of the inner tree serves every configuration of this one, as Ninja Multi-Config does here
    set(spirvToolsConfigurations Debug RelWithDebInfo Release MinSizeRel)
    string(REPLACE ";" "|" spirvToolsConfigurationList "${spirvToolsConfigurations}")

    set(spirvToolsByproducts "")
    foreach (configuration IN LISTS spirvToolsConfigurations)
        list(APPEND spirvToolsByproducts "${spirvToolsBinaryDir}/source/${configuration}/SPIRV-Tools.lib")
    endforeach()

    ExternalProject_Add(lodestone_spirv_tools_build
        SOURCE_DIR "${spirvToolsSourceDir}"
        BINARY_DIR "${spirvToolsBinaryDir}"
        PREFIX "${LODESTONE_ISOLATED_DIR}/spirv-tools"
        DOWNLOAD_COMMAND ""
        UPDATE_COMMAND ""
        LIST_SEPARATOR |
        CMAKE_GENERATOR "Ninja Multi-Config"
        CMAKE_CACHE_ARGS
            "-DCMAKE_CONFIGURATION_TYPES:STRING=${spirvToolsConfigurationList}"
            "-DCMAKE_C_COMPILER:STRING=${CMAKE_C_COMPILER}"
            "-DCMAKE_CXX_COMPILER:STRING=${CMAKE_CXX_COMPILER}"
            "-DSPIRV-Headers_SOURCE_DIR:PATH=${LODESTONE_SLANG_SOURCE_DIR}/external/spirv-headers"
            "-DSPIRV_SKIP_TESTS:BOOL=ON"
            "-DSPIRV_SKIP_EXECUTABLES:BOOL=ON"
            "-DSPIRV_WERROR:BOOL=OFF"
            "-DSPIRV_TOOLS_BUILD_STATIC:BOOL=ON"
            "-DSPIRV_TOOLS_USE_MIMALLOC:BOOL=OFF"
            "-DSKIP_SPIRV_TOOLS_INSTALL:BOOL=ON"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build "${spirvToolsBinaryDir}" --config $<CONFIG> --target SPIRV-Tools-static
        BUILD_ALWAYS ON
        BUILD_BYPRODUCTS ${spirvToolsByproducts}
        INSTALL_COMMAND ""
        EXCLUDE_FROM_ALL ON)

    add_library(SPIRV-Tools-static STATIC IMPORTED GLOBAL)
    foreach (configuration IN LISTS spirvToolsConfigurations)
        string(TOUPPER "${configuration}" configurationUpper)
        set_target_properties(SPIRV-Tools-static PROPERTIES
            "IMPORTED_LOCATION_${configurationUpper}" "${spirvToolsBinaryDir}/source/${configuration}/SPIRV-Tools.lib")
    endforeach()
    set_target_properties(SPIRV-Tools-static PROPERTIES
        IMPORTED_CONFIGURATIONS "${spirvToolsConfigurations}"
        INTERFACE_INCLUDE_DIRECTORIES "${spirvToolsSourceDir}/include")
    add_dependencies(SPIRV-Tools-static lodestone_spirv_tools_build)
endif()
