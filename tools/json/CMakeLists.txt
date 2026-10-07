
set(JSON_WRITER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/JsonWriter.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/JsonWriter.cpp")

add_library(lodestone_json_writer STATIC ${JSON_WRITER_SOURCES})
set_target_properties(lodestone_json_writer PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(lodestone_json_writer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(lodestone_json_writer PUBLIC lodestone::client_library)
add_library(lodestone::json ALIAS lodestone_json_writer)

source_group("tools//json" FILES ${JSON_WRITER_SOURCES})
