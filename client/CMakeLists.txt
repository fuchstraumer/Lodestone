
set(LODESTONE_CLIENT_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/EnumClassUtils.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/ResourceFlags.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/ShaderLibraryTypes.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/ShaderManifest.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/ShaderManifestFormat.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/ShaderManifestIndex.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/ShaderManifestLayout.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/VariantKey.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/VulkanCapabilityTable.hpp")

set(LODESTONE_CLIENT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderManifest.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderManifestLayout.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderManifestRead.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderManifestValidator.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderManifestValidator.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderManifestIndex.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderManifestQuery.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/VariantKey.cpp")

# Lodestone library and tests link against the above, compiled into a library:
# clients link to this (just the headers)
set(LODESTONE_CLIENT_INTERFACE_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/ResourceFlags.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/ShaderLibraryTypes.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/ShaderManifest.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/ShaderManifestFormat.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/ShaderManifestLayout.hpp")

# long name for this one jsut to be consistent: this is target is specifically only for internal
# linkage, not for clients to link against 
# todo-asap: this is not actually true though lol, since we have functions in the headers. oops!
# the objects will need to be made into POD and given a C ABI to really make this work.
add_library(lodestone_client_library STATIC ${LODESTONE_CLIENT_SOURCES} ${LODESTONE_CLIENT_HEADERS})
# pull in the levenshtein distance library for query name suggestions during errors
target_link_libraries(lodestone_client_library PRIVATE lodestone::suggest)
set_target_properties(lodestone_client_library PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(lodestone_client_library PRIVATE
    "${PROJECT_SOURCE_DIR}/client/include"
    "${PROJECT_SOURCE_DIR}/third_party/magic_enum/include"
    "${PROJECT_SOURCE_DIR}/tools/suggest")
add_library(lodestone::client_library ALIAS lodestone_client_library)
if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(lodestone_client_library PRIVATE "$<$<CONFIG:Debug>:-fcolor-diagnostics;-fansi-escape-codes;-fstandalone-debug;-fno-limit-debug-info;-fno-omit-frame-pointer>")
endif()

add_library(lodestone_client_interface INTERFACE)
target_include_directories(lodestone_client_interface INTERFACE "${PROJECT_SOURCE_DIR}/client/include")
add_library(lodestone::client_interface ALIAS lodestone_client_interface)
    
# Header self-containment: one translation unit for each public header, which includes that header and nothing
# else. A header that needs another header first fails to compile here, so the include order of a client can
# never matter. The objects are never linked; building them is the check.
set(LODESTONE_CLIENT_HEADER_CHECK_SOURCES)
foreach(header IN LISTS LODESTONE_CLIENT_HEADERS)
    get_filename_component(headerName "${header}" NAME_WE)
    set(checkSource "${CMAKE_CURRENT_BINARY_DIR}/header_check/${headerName}.cpp")
    file(CONFIGURE OUTPUT "${checkSource}" CONTENT "#include \"${headerName}.hpp\"
")
    list(APPEND LODESTONE_CLIENT_HEADER_CHECK_SOURCES "${checkSource}")
endforeach()
add_library(lodestone_client_header_check OBJECT ${LODESTONE_CLIENT_HEADER_CHECK_SOURCES})
set_target_properties(lodestone_client_header_check PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(lodestone_client_header_check PRIVATE "${PROJECT_SOURCE_DIR}/client/include")
