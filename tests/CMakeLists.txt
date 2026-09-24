# TEST_ARGS passes a command line to the registered test. A test that takes no argument omits it.
function(add_lodestone_unit_test NAME)
    cmake_parse_arguments(UNIT_TEST "" "" "TEST_ARGS" ${ARGN})
    add_executable(${NAME} ${UNIT_TEST_UNPARSED_ARGUMENTS} TestHarness.cpp TestHarness.hpp)

    set_target_properties(${NAME} PROPERTIES FOLDER "Tests")
    set_target_properties(${NAME} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES)

    if (CMAKE_GENERATOR MATCHES "Visual Studio")
        set_target_properties(${NAME} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        set_target_properties(${NAME} PROPERTIES VS_DEBUGGER_COMMAND_ARGUMENTS "${UNIT_TEST_TEST_ARGS}")
    endif()

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
# The same driver, on the probe module that phase E step E0a needed. `EntryPointParams.slang`
# declares a `uniform` parameter on two of its three entry points. A cooker that reads the global
# scope alone gives those parameters no binding, and the cross-check then fails the cook.
#
# The probe cooks on its own, and not beside OceanFft. One cook of two modules writes one shared
# header and one shared dedupe report, so every OceanFft artifact would stop being byte identical.
add_lodestone_unit_test(EntryPointParamsCookTest CookTest.cpp
    TEST_ARGS -o "${CMAKE_CURRENT_BINARY_DIR}/entry_point_params_output"
              --target=wgsl
              --verify-deterministic
              "${CMAKE_SOURCE_DIR}/tests/assets/EntryPointParams.slang")
# The same driver, on the probe module that phase E step E0b needed. `ParameterBlocks.slang` declares
# a block of resources, a block of ordinary data, a block inside a block, and a block on each of two
# entry points. Slang describes a block with descriptor ranges alone, so a cooker that walks the
# binding ranges gives every content of every block no binding at all.
add_lodestone_unit_test(ParameterBlocksCookTest CookTest.cpp
    TEST_ARGS -o "${CMAKE_CURRENT_BINARY_DIR}/parameter_blocks_output"
              --target=wgsl
              --verify-deterministic
              "${CMAKE_SOURCE_DIR}/tests/assets/ParameterBlocks.slang")
# The same driver, on the module that phase E step E7 needed. `InterfaceAxisTest.slang` declares an
# interface axis: an `extern struct SHADE_MODE : IShadeMode` that a per-variant `export struct` makes
# concrete. Its three implementations are tagged `ls_axis_interface_impl` and reach the module through
# an `__include`d fragment, so the axis read must recurse and match the extern to the impls by
# conformance. Crossed with the boolean axis it is a real product of six variants. Exit code 0 states
# that every variant linked, the cross-check agreed, both round trips read back, and two cooks matched.
add_lodestone_unit_test(InterfaceAxisCookTest CookTest.cpp
    TEST_ARGS -o "${CMAKE_CURRENT_BINARY_DIR}/interface_axis_output"
              --target=wgsl
              --verify-deterministic
              "${CMAKE_SOURCE_DIR}/tests/assets/InterfaceAxis/InterfaceAxisTest.slang")
# The same driver, on an enum-axis module. `EnumAxisTest.slang` declares an `[ls_axis_enum]` extern
# const whose Slang enum has explicit, non-ascending case values. The axis read reflects the enum's
# cases (names and values); the manifest stores the case names, and the digit stays the declaration
# order, so a cook that assumed "case index == case value" would produce wrong data. Crossed with a
# boolean axis it is a product of six variants.
add_lodestone_unit_test(EnumAxisCookTest CookTest.cpp
    TEST_ARGS -o "${CMAKE_CURRENT_BINARY_DIR}/enum_axis_output"
              --target=wgsl
              --verify-deterministic
              "${CMAKE_SOURCE_DIR}/tests/assets/EnumAxis/EnumAxisTest.slang")
# The same driver, on the multi-module KitchenSink set. This is the stress and coverage asset: four
# consumer modules cook together against one policy, and KsTypes and KsShading are imported for their
# enum and interface declarations. It crosses every axis kind, groups resources into nested parameter
# blocks, and reaches 2D, 3D, cube, and array textures, a comparison sampler, and raster targets with a
# depth write. It shares axis names across modules with different value subsets.
add_lodestone_unit_test(KitchenSinkCookTest CookTest.cpp
    TEST_ARGS -o "${CMAKE_CURRENT_BINARY_DIR}/kitchen_sink_output"
              --target=wgsl
              --verify-deterministic
              --policy-file "${CMAKE_SOURCE_DIR}/tests/assets/KitchenSink/KitchenSink.toml"
              "${CMAKE_SOURCE_DIR}/tests/assets/KitchenSink/KsGeometry.slang"
              "${CMAKE_SOURCE_DIR}/tests/assets/KitchenSink/KsMaterial.slang"
              "${CMAKE_SOURCE_DIR}/tests/assets/KitchenSink/KsVolume.slang"
              "${CMAKE_SOURCE_DIR}/tests/assets/KitchenSink/KsPost.slang")
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
add_lodestone_unit_test(AttributeExpressionTest AttributeExpressionTests.cpp)
add_lodestone_unit_test(ContentInternerTest ContentInternerTests.cpp)
add_lodestone_unit_test(PermutationIndexTest PermutationIndexTests.cpp)
add_lodestone_unit_test(ShaderManifestRejectTest ShaderManifestRejectTests.cpp)
add_lodestone_unit_test(WgslValidatorTest WgslValidatorTests.cpp)
add_lodestone_unit_test(ReflectionSchemaTest ReflectionSchemaTests.cpp)
add_lodestone_unit_test(DiagnosticParserTest DiagnosticParserTests.cpp)
add_lodestone_unit_test(ResolveStageTest ResolveStageTests.cpp)
add_lodestone_unit_test(StageDumpTest StageDumpTests.cpp)
add_lodestone_unit_test(DedupeInfluenceTest DedupeInfluenceTests.cpp)
add_lodestone_unit_test(PermutationConstraintTest PermutationConstraintTests.cpp)
add_lodestone_unit_test(PolicyDocumentTest PolicyDocumentTests.cpp)
add_lodestone_unit_test(SymbolTableTest SymbolTableTests.cpp)
add_lodestone_unit_test(SuggestTest SuggestTests.cpp)
target_link_libraries(SuggestTest PRIVATE lodestone::suggest)
add_lodestone_unit_test(EnumTagDecodeTest EnumTagDecodeTests.cpp)
add_lodestone_unit_test(ManifestIndexTest ManifestIndexTests.cpp)

# Standalone Slang-only probe (not a unit test). Prints DeclReflection kinds so we can see what an
# interface vs an enum reports, and whether getType() identity holds. It links ONLY slang, so it
# builds while lodestone is mid-refactor. Build it directly: --target DeclKindProbe
add_executable(DeclKindProbe DeclKindProbe.cpp)
set_target_properties(DeclKindProbe PROPERTIES FOLDER "Tests" CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES)
target_include_directories(DeclKindProbe PRIVATE "${CMAKE_SOURCE_DIR}/third_party/slang/include")
target_link_libraries(DeclKindProbe PRIVATE slang)
copy_slang_dlls_to_target(DeclKindProbe)
