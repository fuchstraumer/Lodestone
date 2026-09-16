
set(LODESTONE_CLIENT_HEADERS
    "${CMAKE_SOURCE_DIR}/client/include/EnumClassUtils.hpp"
    "${CMAKE_SOURCE_DIR}/client/include/ResourceFlags.hpp"
    "${CMAKE_SOURCE_DIR}/client/include/ShaderLibraryTypes.hpp"
    "${CMAKE_SOURCE_DIR}/client/include/ShaderManifest.hpp"
    "${CMAKE_SOURCE_DIR}/client/include/ShaderManifestIndex.hpp"
    "${CMAKE_SOURCE_DIR}/client/include/VariantKey.hpp")

set(LODESTONE_CLIENT_SOURCES
    "${CMAKE_SOURCE_DIR}/client/src/ResourceFlags.cpp"
    "${CMAKE_SOURCE_DIR}/client/src/ShaderManifest.cpp"
    "${CMAKE_SOURCE_DIR}/client/src/ShaderManifestIndex.cpp"
    "${CMAKE_SOURCE_DIR}/client/src/VariantKey.cpp")

# Lodestone library and tests link against the above, compiled into a library:
# clients link to this (just the headers)
set(LODESTONE_CLIENT_INTERFACE_HEADERS
    "${CMAKE_SOURCE_DIR}/client/include/ResourceFlags.hpp"
    "${CMAKE_SOURCE_DIR}/client/include/ShaderLibraryTypes.hpp"
    "${CMAKE_SOURCE_DIR}/client/include/ShaderManifest.hpp")

# long name for this one jsut to be consistent: this is target is specifically only for internal
# linkage, not for clients to link against 
# todo-asap: this is not actually true though lol, since we have functions in the headers. oops!
# the objects will need to be made into POD and given a C ABI to really make this work.
add_library(lodestone_client_internal STATIC ${LODESTONE_CLIENT_SOURCES} ${LODESTONE_CLIENT_HEADERS})
# pull in the levenshtein distance library for query name suggestions during errors
target_link_libraries(lodestone_client_internal PRIVATE lodestone::suggest)
set_target_properties(lodestone_client_internal PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(lodestone_client_internal PRIVATE
    "${CMAKE_SOURCE_DIR}/client/include"
    "${CMAKE_SOURCE_DIR}/third_party/magic_enum/include"
    "${CMAKE_SOURCE_DIR}/tools/suggest")
add_library(lodestone::client_internal ALIAS lodestone_client_internal)
if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(lodestone_client_internal PRIVATE "$<$<CONFIG:Debug>:-fcolor-diagnostics;-fansi-escape-codes;-fstandalone-debug;-fno-limit-debug-info;-fno-omit-frame-pointer>")
endif()

add_library(lodestone_client_interface INTERFACE)
target_include_directories(lodestone_client_interface INTERFACE "${CMAKE_SOURCE_DIR}/client/include")
add_library(lodestone::client ALIAS lodestone_client_interface)
    