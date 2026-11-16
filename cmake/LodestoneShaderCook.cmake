# lodestone_add_shader_cook: run one cook as a build step.
#
#   lodestone_add_shader_cook(<name>
#       OUTPUT_DIR <directory>
#       TARGETS <profile>...              # wgsl, spirv: each becomes a --target=
#       MODULES <file.slang>...
#       [POLICY_FILE <file.toml>...]      # merged in the order given; a key that two files set is an error
#       [INCLUDE_DIRS <dir>...]           # searched for imported modules, after the folder of each module
#       [HOST_TYPES <include>]            # also write ShaderLibrary.host.hpp. <include> is the text of the #include
#                                         # that the header uses for float2 to float4x4 and friends
#       [OPTIONS <cooker argument>...])   # passed to the cooker as written, for example --quiet
#
# This adds a custom target <name>. Use add_dependencies to make the consuming target depend on it. The
# target property LODESTONE_MANIFEST stores the path of the cooked manifest.
#
# HOST_TYPES turns on the host header: the C++ mirrors of the shader structs and the binding names. The cook name
# is the C++ namespace of the header, so it must be a C++ identifier. The target property LODESTONE_HOST_HEADER
# stores the path of the header. The cooker leaves a header with unchanged text alone, and the header is a byproduct
# of the cook step, so Ninja skips the steps that only depend on it. The consumer must find <include> through its
# own include directories, and must find ShaderLibraryTypes.hpp of the client library.
#
# The cook re-runs when a module, the policy file, the cooker, or any file imported or included by a module
# changes. The cooker writes a depfile (`--depfile`) that lists every file Slang read, so that consumers do
# not need to maintain a list of included files by hand.
#
# By default this runs the lodestone_cooker_console target of this build. Builds without a cooker
# (LODESTONE_BUILD_COOKER OFF, as in Emscripten builds) must set LODESTONE_COOKER_EXECUTABLE to a host cooker.
# When that variable is set, it is used instead of the target.

set(LODESTONE_COOKER_EXECUTABLE "" CACHE FILEPATH
    "A host lodestone_cooker_console executable. Required when this build has no cooker, as in a cross build.")

function(lodestone_add_shader_cook NAME)
    cmake_parse_arguments(PARSE_ARGV 1 COOK "" "OUTPUT_DIR;HOST_TYPES" "TARGETS;MODULES;POLICY_FILE;OPTIONS;INCLUDE_DIRS")
    if (COOK_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "lodestone_add_shader_cook(${NAME}): unknown arguments: ${COOK_UNPARSED_ARGUMENTS}")
    endif()
    foreach (REQUIRED_ARGUMENT OUTPUT_DIR TARGETS MODULES)
        if (NOT COOK_${REQUIRED_ARGUMENT})
            message(FATAL_ERROR "lodestone_add_shader_cook(${NAME}): ${REQUIRED_ARGUMENT} is required.")
        endif()
    endforeach()

    if (LODESTONE_COOKER_EXECUTABLE)
        set(COOKER_COMMAND "${LODESTONE_COOKER_EXECUTABLE}")
        set(COOKER_DEPENDENCY "${LODESTONE_COOKER_EXECUTABLE}")
    elseif (TARGET lodestone_cooker_console)
        set(COOKER_COMMAND "$<TARGET_FILE:lodestone_cooker_console>")
        set(COOKER_DEPENDENCY lodestone_cooker_console)
    else()
        message(FATAL_ERROR "lodestone_add_shader_cook(${NAME}): this build has no cooker. "
                            "Set LODESTONE_COOKER_EXECUTABLE to a host lodestone_cooker_console.")
    endif()

    if (COOK_HOST_TYPES AND NOT NAME MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "lodestone_add_shader_cook(${NAME}): HOST_TYPES uses the name as a C++ namespace, "
                            "so the name must be a C++ identifier.")
    endif()

    # resolve relative paths against the calling directory, the same way add_custom_command does
    cmake_path(ABSOLUTE_PATH COOK_OUTPUT_DIR BASE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" NORMALIZE)
    set(MODULE_PATHS "")
    foreach (MODULE IN LISTS COOK_MODULES)
        cmake_path(ABSOLUTE_PATH MODULE BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
        list(APPEND MODULE_PATHS "${MODULE}")
    endforeach()

    set(COOK_ARGUMENTS -o "${COOK_OUTPUT_DIR}" --depfile)
    foreach (PROFILE IN LISTS COOK_TARGETS)
        list(APPEND COOK_ARGUMENTS "--target=${PROFILE}")
    endforeach()
    set(INPUT_DEPENDENCIES ${MODULE_PATHS})
    foreach (POLICY_FILE IN LISTS COOK_POLICY_FILE)
        cmake_path(ABSOLUTE_PATH POLICY_FILE BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
        list(APPEND COOK_ARGUMENTS --policy-file "${POLICY_FILE}")
        list(APPEND INPUT_DEPENDENCIES "${POLICY_FILE}")
    endforeach ()
    foreach (INCLUDE_DIR IN LISTS COOK_INCLUDE_DIRS)
        cmake_path(ABSOLUTE_PATH INCLUDE_DIR BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
        list(APPEND COOK_ARGUMENTS --include-dir "${INCLUDE_DIR}")
    endforeach ()
    set(HOST_HEADER_PATH "")
    if (COOK_HOST_TYPES)
        set(HOST_HEADER_PATH "${COOK_OUTPUT_DIR}/ShaderLibrary.host.hpp")
        list(APPEND COOK_ARGUMENTS "--host-types=${COOK_HOST_TYPES}" "--host-namespace=${NAME}")
    endif()
    list(APPEND COOK_ARGUMENTS ${COOK_OPTIONS} ${MODULE_PATHS})

    set(MANIFEST_PATH "${COOK_OUTPUT_DIR}/ShaderLibrary.ldmanifest")
    add_custom_command(
        OUTPUT "${MANIFEST_PATH}" "${COOK_OUTPUT_DIR}/ShaderLibrary.dedupe.txt"
        BYPRODUCTS ${HOST_HEADER_PATH}
        COMMAND "${COOKER_COMMAND}" ${COOK_ARGUMENTS}
        DEPENDS ${INPUT_DEPENDENCIES} ${COOKER_DEPENDENCY}
        DEPFILE "${COOK_OUTPUT_DIR}/ShaderLibrary.d"
        COMMENT "Cooking shaders: ${NAME}"
        VERBATIM
        COMMAND_EXPAND_LISTS)
    add_custom_target(${NAME} DEPENDS "${MANIFEST_PATH}")
    set_target_properties(${NAME} PROPERTIES LODESTONE_MANIFEST "${MANIFEST_PATH}")
    if (COOK_HOST_TYPES)
        set_target_properties(${NAME} PROPERTIES LODESTONE_HOST_HEADER "${HOST_HEADER_PATH}")
    endif()
endfunction()
