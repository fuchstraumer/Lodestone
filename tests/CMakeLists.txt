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
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/client/include")

    target_link_libraries(${NAME} PRIVATE lodestone slang lodestone::client_library)
    if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${NAME} PRIVATE "$<$<CONFIG:Debug>:-fcolor-diagnostics;-fansi-escape-codes;-fstandalone-debug;-fno-limit-debug-info;-fno-omit-frame-pointer>")
    endif()
    add_test(NAME ${NAME} COMMAND ${NAME} ${UNIT_TEST_TEST_ARGS})
    copy_slang_dlls_to_target(${NAME})
endfunction()
# The same driver, on the probe module added for the entry point scope walk. `EntryPointParams.slang`
# declares a `uniform` parameter on two of its three entry points. A cooker that reads the global
# scope alone gives those parameters no binding, and the cross-check then fails the cook.
#
# The probe cooks on its own, and not beside OceanFft. One cook of two modules writes one shared
# header and one shared dedupe report, so every OceanFft artifact would stop being byte identical.
add_lodestone_unit_test(EntryPointParamsCookTest CookTest.cpp
    TEST_ARGS -o "${CMAKE_CURRENT_BINARY_DIR}/entry_point_params_output"
              --target=wgsl
              --target=spirv
              --verify-deterministic
              "${PROJECT_SOURCE_DIR}/tests/assets/EntryPointParams.slang")
# The same driver, on the probe module added for the parameter block walk. `ParameterBlocks.slang` declares
# a block of resources, a block of ordinary data, a block inside a block, and a block on each of two
# entry points. Slang describes a block with descriptor ranges alone, so a cooker that walks the
# binding ranges gives every content of every block no binding at all.
add_lodestone_unit_test(ParameterBlocksCookTest CookTest.cpp
    TEST_ARGS -o "${CMAKE_CURRENT_BINARY_DIR}/parameter_blocks_output"
              --target=wgsl
              --target=spirv
              --verify-deterministic
              "${PROJECT_SOURCE_DIR}/tests/assets/ParameterBlocks.slang")
# Two modules in separate folders, cooked in one run: the kernel imports the other module, which is outside its folder and
# its parent. Without --include-dir the cook fails (it once failed in worker setup). The shared module comes first, so the
# kernel finds a cached copy of it, which a worker cannot load.
add_lodestone_unit_test(CrossModuleCookTest CookTest.cpp
    TEST_ARGS -o "${CMAKE_CURRENT_BINARY_DIR}/cross_module_output"
              --target=spirv
              --include-dir "${PROJECT_SOURCE_DIR}/tests/assets/CrossModule/lib"
              --verify-deterministic
              "${PROJECT_SOURCE_DIR}/tests/assets/CrossModule/lib/CmShared.slang"
              "${PROJECT_SOURCE_DIR}/tests/assets/CrossModule/app/CmKernel.slang")
# The workgroup memory probe: four groupshared variables that pad differently, and one half4 array. The cross-profile
# validator fails the cook when the two targets read different bytes or a different 16-bit float requirement.
add_lodestone_unit_test(WorkgroupMemoryCookTest CookTest.cpp
    TEST_ARGS -o "${CMAKE_CURRENT_BINARY_DIR}/workgroup_memory_output"
              --target=wgsl
              --target=spirv
              --verify-deterministic
              "${PROJECT_SOURCE_DIR}/tests/assets/WorkgroupMemory.slang")
# The same driver, on the module added with interface axes. `InterfaceAxisTest.slang` declares an
# interface axis: an `extern struct SHADE_MODE : IShadeMode` that a per-variant `export struct` makes
# concrete. Its three implementations are tagged `ls_axis_interface_impl` and reach the module through
# an `__include`d fragment, so the axis read must recurse and match the extern to the impls by
# conformance. Crossed with the boolean axis it is a real product of six variants. Exit code 0 means
# that every variant linked, the cross-check agreed, both round trips read back, and two cooks matched.
add_lodestone_unit_test(InterfaceAxisCookTest CookTest.cpp
    TEST_ARGS -o "${CMAKE_CURRENT_BINARY_DIR}/interface_axis_output"
              --target=wgsl
              --target=spirv
              --verify-deterministic
              "${PROJECT_SOURCE_DIR}/tests/assets/InterfaceAxis/InterfaceAxisTest.slang")
# The same driver, on an enum-axis module. `EnumAxisTest.slang` declares an `[ls_axis_enum]` extern
# const whose Slang enum has explicit, non-ascending case values. The axis read reflects the enum's
# cases (names and values); the manifest stores the case names, and the digit stays the declaration
# order, so a cook that assumed "case index == case value" would produce wrong data. Crossed with a
# boolean axis it is a product of six variants.
add_lodestone_unit_test(EnumAxisCookTest CookTest.cpp
    TEST_ARGS -o "${CMAKE_CURRENT_BINARY_DIR}/enum_axis_output"
              --target=wgsl
              --target=spirv
              --verify-deterministic
              "${PROJECT_SOURCE_DIR}/tests/assets/EnumAxis/EnumAxisTest.slang")
# The same driver, on the multi-module KitchenSink set. This is the stress and coverage asset: four
# consumer modules cook together against one policy, and KsTypes and KsShading are imported for their
# enum and interface declarations. It crosses every axis kind, groups resources into nested parameter
# blocks, and reaches 2D, 3D, cube, and array textures, a comparison sampler, and raster targets with a
# depth write. It shares axis names across modules with different value subsets.
add_lodestone_unit_test(KitchenSinkCookTest CookTest.cpp
    TEST_ARGS -o "${CMAKE_CURRENT_BINARY_DIR}/kitchen_sink_output"
              --target=wgsl
              --target=spirv
              --verify-deterministic
              --policy-file "${PROJECT_SOURCE_DIR}/tests/assets/KitchenSink/KitchenSink.toml"
              "${PROJECT_SOURCE_DIR}/tests/assets/KitchenSink/KsGeometry.slang"
              "${PROJECT_SOURCE_DIR}/tests/assets/KitchenSink/KsMaterial.slang"
              "${PROJECT_SOURCE_DIR}/tests/assets/KitchenSink/KsVolume.slang"
              "${PROJECT_SOURCE_DIR}/tests/assets/KitchenSink/KsPost.slang")
# The one test in this suite that asserts a cook FAILS.
#
# Every other cook test checks that a good module cooks. A check that never fires and a check that
# always fires both leave those tests green, so only a module that must fail can separate them.
# The table in the source contains a control row for that reason.
#
# CMake writes the asset directory in, so the test needs no command line and the run-tests script
# can run it directly. The table refers to a file inside that directory, so a later rejection costs one
# module and one line, and no new target.
add_lodestone_unit_test(AccessModelRejectTest AccessModelRejectTests.cpp)
target_compile_definitions(AccessModelRejectTest PRIVATE
    LODESTONE_TEST_ASSET_DIR="${PROJECT_SOURCE_DIR}/tests/assets")
add_lodestone_unit_test(AttributeExpressionTest AttributeExpressionTests.cpp)
# The declared default of each axis, from the shader initializer to the client query layer. It cooks through
# Slang, so it takes about one second.
add_lodestone_unit_test(AxisDefaultTest AxisDefaultTests.cpp)
target_compile_definitions(AxisDefaultTest PRIVATE
    LODESTONE_TEST_ASSET_DIR="${PROJECT_SOURCE_DIR}/tests/assets/AxisDefaults")
# Derived constants in size expressions, from the shader source to the element count in the manifest. It cooks
# through Slang, so it takes about one second.
add_lodestone_unit_test(DerivedConstantTest DerivedConstantTests.cpp)
target_compile_definitions(DerivedConstantTest PRIVATE
    LODESTONE_TEST_ASSET_DIR="${PROJECT_SOURCE_DIR}/tests/assets/DerivedConstants")
# The entry point constants of both targets, from the shader source to the manifest, against hand-computed layouts.
# It cooks through Slang, so it takes about one second.
add_lodestone_unit_test(EntryPointConstantsTest EntryPointConstantsTests.cpp)
target_compile_definitions(EntryPointConstantsTest PRIVATE
    LODESTONE_TEST_ASSET_DIR="${PROJECT_SOURCE_DIR}/tests/assets/EntryPointConstants")
# The resource roles and tags (task L17), from the shader source to the manifest, and each rule's failure. It cooks
# through Slang, so it takes about one second.
add_lodestone_unit_test(ResourceRoleTest ResourceRoleTests.cpp)
target_compile_definitions(ResourceRoleTest PRIVATE
    LODESTONE_TEST_ASSET_DIR="${PROJECT_SOURCE_DIR}/tests/assets/ResourceRoles")
# Fixed-size binding arrays (DiamondDogs request C12) for SPIR-V, from the shader source to the manifest, and the two forms that
# must still fail: WGSL, and an unbounded array. It cooks through Slang, so it takes about one second.
add_lodestone_unit_test(BindingArrayTest BindingArrayTests.cpp)
target_compile_definitions(BindingArrayTest PRIVATE
    LODESTONE_TEST_ASSET_DIR="${PROJECT_SOURCE_DIR}/tests/assets/BindingArrays")
# Axes of kind capability (phase F step F3.5): an axis must change the capabilities, and its off value must not need more than its
# on value. Hand-built rows for each rule, and cooks that must pass or fail on both targets. It takes about two seconds.
add_lodestone_unit_test(CapabilityAxisTest CapabilityAxisTests.cpp)
target_compile_definitions(CapabilityAxisTest PRIVATE
    LODESTONE_TEST_ASSET_DIR="${PROJECT_SOURCE_DIR}/tests/assets/CapabilityAxes")
# The shim module (phase F step F2.1): the Bound implementations cook for both targets, and each resource reads back with
# the kind and placement of the resource it holds. It cooks through Slang, so it takes about one second.
add_lodestone_unit_test(ShimModuleTest ShimModuleTests.cpp)
target_compile_definitions(ShimModuleTest PRIVATE
    LODESTONE_TEST_ASSET_DIR="${PROJECT_SOURCE_DIR}/tests/assets/Shims")
# The shim table agrees with the shim module (decision D6, phase F step F2.2): every cell names a type that exists, takes the
# right arity, and conforms to its interface, and a table that disagrees fails. It cooks through Slang four times.
add_lodestone_unit_test(ShimTableTest ShimTableTests.cpp)
# Shim externs (phase F step F2.3): the symbol table reads `extern struct X : IReadBuffer<T>` from the source, reflection confirms it,
# and the extern stays out of the axis space. It starts two bootstrap compiles, so it takes about one second.
add_lodestone_unit_test(ShimExternTest ShimExternTests.cpp)
target_compile_definitions(ShimExternTest PRIVATE
    LODESTONE_TEST_ASSET_DIR="${PROJECT_SOURCE_DIR}/tests/assets/Shims")
# The source of the module that links the shim implementations (phase F step F2.5a): imports, exports, and the errors for an empty
# cell and for type arguments that are not plain names. It needs no Slang.
add_lodestone_unit_test(ShimLinkSourceTest ShimLinkSourceTests.cpp)
# Shim externs end to end (phase F steps F2.5 and F2.6): a material written with shims is compared with the same shader written with the
# resource types, for both targets, and a shim used as a local must fail. It cooks through Slang, so it takes about three seconds.
add_lodestone_unit_test(ShimCookTest ShimCookTests.cpp)
target_compile_definitions(ShimCookTest PRIVATE
    LODESTONE_TEST_ASSET_DIR="${PROJECT_SOURCE_DIR}/tests/assets/Shims")
# The storage formats that every target accepts (DiamondDogs request C11, step C11.1): the enum in the shim module, the table in
# target/, and the three readers of a texture format must agree. It cooks one probe for both targets, so it takes about one second.
add_lodestone_unit_test(StorageFormatTest StorageFormatTests.cpp)
target_compile_definitions(StorageFormatTest PRIVATE
    LODESTONE_TEST_ASSET_DIR="${PROJECT_SOURCE_DIR}/tests/assets/Shims")
# A mip kernel whose storage format is an axis (DiamondDogs request C11, step C11.4), compared with one control for each format
# written with the literal type, and a policy that cooks a subset. It cooks through Slang four times, so it takes a few seconds.
add_lodestone_unit_test(StorageFormatAxisTest StorageFormatAxisTests.cpp)
target_compile_definitions(StorageFormatAxisTest PRIVATE
    LODESTONE_TEST_ASSET_DIR="${PROJECT_SOURCE_DIR}/tests/assets/StorageFormatAxis")
# A canary for shader-slang/slang#13789: it fails when Slang fixes the issue, so the workaround cannot outlive it.
add_lodestone_unit_test(SlangMetadataCanaryTest SlangMetadataCanaryTests.cpp)
add_lodestone_unit_test(ContentInternerTest ContentInternerTests.cpp)
add_lodestone_unit_test(PermutationIndexTest PermutationIndexTests.cpp)
add_lodestone_unit_test(ShaderManifestRejectTest ShaderManifestRejectTests.cpp)
if (LODESTONE_ENABLE_WGSL)
    add_lodestone_unit_test(WgslValidatorTest WgslValidatorTests.cpp)
endif()
# The SPIR-V cross-check. The test assembles its fixture with the SPIRV-Tools assembler, so it links
# SPIRV-Tools itself. No switch: Slang builds SPIRV-Tools in every configuration.
add_lodestone_unit_test(SpvValidatorTest SpvValidatorTests.cpp)
target_link_libraries(SpvValidatorTest PRIVATE SPIRV-Tools-static)
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
add_lodestone_unit_test(MergedLayoutTest MergedLayoutTests.cpp)
add_lodestone_unit_test(ResourceReportTest ResourceReportTests.cpp)

# Standalone Slang-only probe (not a unit test). Prints DeclReflection kinds so we can see what an
# interface vs an enum reports, and whether getType() identity holds. It links ONLY slang, so it
# builds while lodestone is mid-refactor. Build it directly: --target DeclKindProbe
add_executable(DeclKindProbe DeclKindProbe.cpp)
set_target_properties(DeclKindProbe PROPERTIES FOLDER "Tests" CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES)
target_include_directories(DeclKindProbe PRIVATE "${PROJECT_SOURCE_DIR}/third_party/slang/include")
target_link_libraries(DeclKindProbe PRIVATE slang)
copy_slang_dlls_to_target(DeclKindProbe)

# Standalone Slang-only probe (not a unit test). It reproduces the Debug-only Slang assert
# "duplicate global instruction" on the modules in tests/assets/SlangPrelinkRepro. Run it after a
# Slang update: exit code 0 means the fault is gone. Build it directly: --target SlangPrelinkRepro
add_executable(SlangPrelinkRepro SlangPrelinkRepro.cpp)
set_target_properties(SlangPrelinkRepro PROPERTIES FOLDER "Tests" CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES)
target_include_directories(SlangPrelinkRepro PRIVATE "${PROJECT_SOURCE_DIR}/third_party/slang/include")
target_link_libraries(SlangPrelinkRepro PRIVATE slang)
copy_slang_dlls_to_target(SlangPrelinkRepro)
