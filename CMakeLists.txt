cmake_minimum_required(VERSION 3.24) # need multiconfig support
project(lodestone LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# cmake/ contains our own FindVulkanHeaders.cmake; must be set before any find_package calls
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(Vulkan REQUIRED)
set(VulkanHeaders_INCLUDE_DIR ${Vulkan_INCLUDE_DIR} CACHE PATH "" FORCE)

include(cmake/ConfigureBuildFlags.cmake)

if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND NOT EMSCRIPTEN)
    if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "Clang")
        message(FATAL_ERROR "Clang-CL in MSVC frontend variant is not supported. Please use a different compiler.")
    else()
        message(STATUS "Using MSVC build flags and tools")
        add_msvc_exclusive_build_flags_all_targets()
        add_shared_msvc_style_build_flags_all_targets()
    endif()
elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT EMSCRIPTEN)
    if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        # Running as clang, but with MSVC frontend (clang-cl as shim layer)
        message(STATUS "Using clang-cl")
        add_shared_msvc_style_build_flags_all_targets()
        disable_msvc_frontend_clang_compiler_warnings_all_targets()
    else()
        message(STATUS "Using Fully-Clang-Based build flags and tools")
        add_shared_clang_style_build_flags_all_targets()
        add_clang_style_build_flags_all_targets()
    endif()
else()
    message(STATUS
        "Could not find supported compiler. Please use Clang, MSVC, or Emscripten and make sure they are on your PATH.")
    message(FATAL_ERROR "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()

include(cmake/ConfigureSlang.cmake)

# xxHash runs a plain option(BUILD_SHARED_LIBS ... ON), which writes ON into the cache. Its own
# CMAKE_DEPENDENT_OPTION then forces the value back to OFF for xxHash only, as a directory variable,
# so the cache entry stays ON for every subdirectory added after this one. Slang reads that entry for
# its bundled miniz, builds miniz as a DLL, and marks every miniz symbol dllimport. miniz is
# EXCLUDE_FROM_ALL and no rule copies miniz.dll, so slang-bootstrap.exe then fails to load with
# STATUS_DLL_NOT_FOUND and the core module never generates. Claim the cache entry first to stop this.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build third party dependencies as static libraries" FORCE)

set(XXHASH_BUILD_XXHSUM OFF)
add_subdirectory(third_party/xxHash/build/cmake EXCLUDE_FROM_ALL)

add_slang_subdirectory(third_party/slang)
set_target_properties(slang PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# Add our various internal libraries first: the client library, and our
# JSON writer that is used by some of the .exe tools and the cooker lib
add_subdirectory(tools/json)
add_subdirectory(client)

set(LODESTONE_CLIENT_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/client/include/ResourceFlags.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/client/include/ShaderLibraryTypes.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/client/include/ShaderManifest.hpp")

set(LODESTONE_CLIENT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/client/src/ResourceFlags.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/client/src/ShaderLibraryTypes.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/client/src/ShaderManifest.cpp")

set(LODESTONE_COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/include/CookerErrors.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/CookerErrors.cpp")

set(LODESTONE_COMPILE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/include/compile/Diagnostics.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/compile/RawLibrary.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/compile/SlangCompiler.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/compile/SlangDiagnosticParser.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/compile/Diagnostics.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/compile/RawLibrary.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/compile/SlangCompiler.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/compile/SlangDiagnosticParser.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/compile/impl/SlangCompilerImpl.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/compile/impl/SlangCompilerImpl.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/compile/impl/SlangReflector.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/compile/impl/SlangReflector.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/compile/impl/ThreadPool.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/compile/impl/ThreadPool.cpp")

set(LODESTONE_DRIVER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/include/driver/CookerDriver.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/driver/CookerOptions.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/driver/CookerDriver.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/driver/CookerOptions.cpp")

set(LODESTONE_EMIT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/include/emit/DedupeReport.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/emit/OutputSink.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/emit/ShaderLibraryEmitter.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/emit/ShaderManifestEmitter.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/emit/StageDump.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/emit/DedupeReport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/emit/OutputSink.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/emit/ShaderLibraryEmitter.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/emit/ShaderManifestEmitter.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/emit/StageDump.cpp")

set(LODESTONE_MODEL_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/include/model/ContentHash.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/model/ContentInterner.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/model/CookedLibrary.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/model/ResolveStage.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/model/ShaderDataSchema.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/model/ContentHash.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/model/CookedLibrary.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/model/ResolveStage.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/model/ShaderDataSchema.cpp")

set(LODESTONE_PERMUTE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/include/permute/ExternConstantScanner.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/permute/PermutationAssignment.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/permute/PermutationAxis.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/permute/PermutationPolicy.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/permute/PermutationRegistry.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/permute/PermutationSpace.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/permute/PermutationValue.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/permute/SizeExpression.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/permute/ExternConstantScanner.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/permute/PermutationAssignment.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/permute/PermutationAxis.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/permute/PermutationRegistry.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/permute/PermutationSpace.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/permute/PermutationValue.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/permute/SizeExpression.cpp")

set(LODESTONE_TARGET_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/include/target/TargetProfile.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/target/WgslBindingScanner.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/target/TargetProfile.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/target/WgslBindingScanner.cpp")


# Static on purpose. No header marks a symbol dllexport, so a Windows DLL build of this target exports
# nothing and every consumer fails to link. A SHARED build is a temporary measure for instrumented
# performance analysis only, and it needs WINDOWS_EXPORT_ALL_SYMBOLS to link at all.
add_library(lodestone STATIC ${LODESTONE_COMMON_SOURCES} ${LODESTONE_COMPILE_SOURCES} ${LODESTONE_DRIVER_SOURCES}
                             ${LODESTONE_EMIT_SOURCES} ${LODESTONE_MODEL_SOURCES} ${LODESTONE_PERMUTE_SOURCES}
                             ${LODESTONE_TARGET_SOURCES})
add_library(lodestone::lodestone ALIAS lodestone)
set_target_properties(lodestone PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
# The engine include path carries shader/ShaderLibraryTypes.hpp and resource/ResourceFlags.hpp. The
# cooker and the rendergraph share that vocabulary, so neither can drift from the other. Neither
# header pulls in Dawn: ResourceFlagConversions.hpp is the only file that names a WebGPU type.
target_include_directories(lodestone PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/client/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/slang/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/magic_enum/include")
target_link_libraries(lodestone PRIVATE slang lodestone::client_internal xxHash::xxhash lodestone::json)
target_compile_options(lodestone PUBLIC "$<$<CONFIG:RelWithDebInfo>:/Ob2;/arch:AVX2>")
if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_definitions(lodestone PRIVATE "_HAS_CXX23=1")
    # No sanitizer here. AddressSanitizer on Windows does not support the debug CRT: it intercepts
    # malloc and free, ucrtbased.dll allocates through _malloc_dbg, and ASan then reports the teardown
    # free as a bad free and aborts before main. To turn the sanitizers on, every target in the
    # process must first use the release CRT, slang included.
    target_compile_options(lodestone PRIVATE "$<$<CONFIG:Debug>:-fcolor-diagnostics;-fansi-escape-codes;-fstandalone-debug;-fno-limit-debug-info;-fno-omit-frame-pointer>")
endif()

source_group("common" FILES ${LODESTONE_COMMON_SOURCES})
source_group("compile" FILES ${LODESTONE_COMPILE_SOURCES})
source_group("driver" FILES ${LODESTONE_DRIVER_SOURCES})
source_group("emit" FILES ${LODESTONE_EMIT_SOURCES})
source_group("model" FILES ${LODESTONE_MODEL_SOURCES})
source_group("permute" FILES ${LODESTONE_PERMUTE_SOURCES})
source_group("target" FILES ${LODESTONE_TARGET_SOURCES})
source_group("client" FILES ${LODESTONE_CLIENT_HEADERS} ${LODESTONE_CLIENT_SOURCES})

copy_slang_dlls_to_target(lodestone)

option(LODESTONE_BUILD_TESTS "Build the standalone unit test executables" ON)

if(LODESTONE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

OPTION(LODESTONE_BUILD_TOOLS "Build the standalone tools" ON)

if(LODESTONE_BUILD_TOOLS)
    add_subdirectory(tools/manifest_dump)
    add_subdirectory(tools/cooker_console)
endif()
