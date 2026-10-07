
set(MANIFEST_DUMP_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")

add_executable(manifest_dump ${MANIFEST_DUMP_SOURCES})
set_target_properties(manifest_dump PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# Only needs the client-facing manifest reader + shader vocabulary, not the cooker or Slang itself.
target_include_directories(manifest_dump PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/client/include"
    "${CMAKE_SOURCE_DIR}/third_party/magic_enum/include")
target_link_libraries(manifest_dump PRIVATE lodestone::client_library lodestone::json)

if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(manifest_dump PRIVATE "$<$<CONFIG:Debug>:-fcolor-diagnostics;-fansi-escape-codes;-fstandalone-debug;-fno-limit-debug-info;-fno-omit-frame-pointer>")
endif()

source_group("tools//manifest_dump" FILES ${MANIFEST_DUMP_SOURCES})
