# TEST_ARGS passes a command line to the registered test. A test that takes no argument omits it.
function(add_lodestone_unit_test NAME)
    cmake_parse_arguments(UNIT_TEST "" "" "TEST_ARGS" ${ARGN})
    add_executable(${NAME} ${UNIT_TEST_UNPARSED_ARGUMENTS} TestHarness.cpp TestHarness.hpp)

    set_target_properties(${NAME} PROPERTIES FOLDER "Tests")
    set_target_properties(${NAME} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES)

    # Both paths are here because a test includes a cooker header and a client header by name only,
    # the way the library's own sources do.
    target_include_directories(${NAME} PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/client/include")

    target_link_libraries(${NAME} PRIVATE lodestone slang lodestone::client_internal)
    if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${NAME} PRIVATE "$<$<CONFIG:Debug>:-fcolor-diagnostics;-fansi-escape-codes;-fstandalone-debug;-fno-limit-debug-info;-fno-omit-frame-pointer>")
    endif()
    add_test(NAME ${NAME} COMMAND ${NAME} ${UNIT_TEST_TEST_ARGS})
    copy_slang_dlls_to_target(${NAME})
endfunction()

# CookTest is the cooker driver, so it needs a real module to cook. It proves the whole pipeline: the
# cook fails on a reflection mismatch, on a failed round trip, and on a non-deterministic result, so
# exit code 0 is a real statement about the output.
add_lodestone_unit_test(CookTest CookTest.cpp
    TEST_ARGS -o "${CMAKE_CURRENT_BINARY_DIR}/cook_test_output/ShaderLibrary.hpp"
              --verify-deterministic
              "${CMAKE_SOURCE_DIR}/tests/assets/compute/Ocean/OceanFft.slang")
# The same driver, on the probe module that phase E step E0a needed. `EntryPointParams.slang`
# declares a `uniform` parameter on two of its three entry points. A cooker that reads the global
# scope alone gives those parameters no binding, and the cross-check then fails the cook.
#
# The probe cooks on its own, and not beside OceanFft. One cook of two modules writes one shared
# header and one shared dedupe report, so every OceanFft artifact would stop being byte identical.
add_lodestone_unit_test(EntryPointParamsCookTest CookTest.cpp
    TEST_ARGS -o "${CMAKE_CURRENT_BINARY_DIR}/entry_point_params_output/ShaderLibrary.hpp"
              --verify-deterministic
              "${CMAKE_SOURCE_DIR}/tests/assets/EntryPointParams.slang")
# The same driver, on the probe module that phase E step E0b needed. `ParameterBlocks.slang` declares
# a block of resources, a block of ordinary data, a block inside a block, and a block on each of two
# entry points. Slang describes a block with descriptor ranges alone, so a cooker that walks the
# binding ranges gives every content of every block no binding at all.
add_lodestone_unit_test(ParameterBlocksCookTest CookTest.cpp
    TEST_ARGS -o "${CMAKE_CURRENT_BINARY_DIR}/parameter_blocks_output/ShaderLibrary.hpp"
              --verify-deterministic
              "${CMAKE_SOURCE_DIR}/tests/assets/ParameterBlocks.slang")
# The one test in this suite that asserts a cook FAILS. Phase E step E0c needed it.
#
# Every other cook test states that a good module cooks. A check that never fires and a check that
# always fires both leave those tests green, so only a module that must fail can separate them.
# The table in the source holds a control row for that reason.
#
# CMake writes the asset directory in, so the test needs no command line and the run-tests script
# can run it directly. The table names a file inside that directory, so a later rejection costs one
# module and one line, and no new target.
add_lodestone_unit_test(AccessModelRejectTest AccessModelRejectTests.cpp)
target_compile_definitions(AccessModelRejectTest PRIVATE
    LODESTONE_TEST_ASSET_DIR="${CMAKE_SOURCE_DIR}/tests/assets")
add_lodestone_unit_test(ExternConstantScannerTest ExternConstantScannerTests.cpp)
add_lodestone_unit_test(SizeExpressionTest SizeExpressionTests.cpp)
add_lodestone_unit_test(ContentInternerTest ContentInternerTests.cpp)
add_lodestone_unit_test(PermutationIndexTest PermutationIndexTests.cpp)
add_lodestone_unit_test(ShaderManifestRejectTest ShaderManifestRejectTests.cpp)
add_lodestone_unit_test(WgslBindingScannerTest WgslBindingScannerTests.cpp)
add_lodestone_unit_test(DiagnosticParserTest DiagnosticParserTests.cpp)
add_lodestone_unit_test(ResolveStageTest ResolveStageTests.cpp)
add_lodestone_unit_test(StageDumpTest StageDumpTests.cpp)
add_lodestone_unit_test(DedupeInfluenceTest DedupeInfluenceTests.cpp)
