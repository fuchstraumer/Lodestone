
set(SUGGEST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Suggest.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Suggest.cpp")

add_library(lodestone_suggest STATIC ${SUGGEST_SOURCES})
set_target_properties(lodestone_suggest PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(lodestone_suggest PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
add_library(lodestone::suggest ALIAS lodestone_suggest)

source_group("tools//suggest" FILES ${SUGGEST_SOURCES})
