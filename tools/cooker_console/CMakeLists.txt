
set(LODESTONE_COOKER_CONSOLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/CookerConsole.cpp")
add_executable(lodestone_cooker_console ${LODESTONE_COOKER_CONSOLE_SOURCES})
set_target_properties(lodestone_cooker_console PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(lodestone_cooker_console PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/tools/shared"
    "${CMAKE_SOURCE_DIR}/client/include")
target_link_libraries(lodestone_cooker_console PRIVATE lodestone::lodestone lodestone::client_internal)
copy_slang_dlls_to_target(lodestone_cooker_console)