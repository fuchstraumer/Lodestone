# Sets flags for use *only* with the MSVC compiler end-to-end. Mostly just /MP right now
function(add_msvc_exclusive_build_flags_all_targets)   
    # Flags not related to perf, but to making things work at baseline ime
    # /MP: Tells cl.exe to multithread. By default, .obj files are compiled serially
    # /bigobj: Increases number of sections in .obj (slang can bump into this ime)
    # /arch:AVX2: Enables AVX2 instructions (helps get math to vectorize better in Math.hpp)
    add_compile_options(/MP /bigobj)
    add_compile_definitions(
        "$<$<CONFIG:RelWithDebInfo,Release>:_HAS_EXCEPTIONS=0>")
endfunction()

# Sets MSVC style flags for the current directory and all subdirectories, useful for when
# we want subdirs to inherit various optimization or core flags that CMake is less likely to set
# Uses generator functions so it should work with multi-config generators as well
function(add_shared_msvc_style_build_flags_all_targets)
    # relwithdebinfo uses more individual granular flags, but lower overall level + /Zo
    # that should help keep more debug info around despite the optimization level
    # rel is just /O2 with a few extras. Note /Ob3 for absolute max inlining
    add_compile_options(
        "$<$<CONFIG:Debug>:/Od;/Zi;/EHsc>"
        "$<$<CONFIG:RelWithDebInfo>:/Ob1;/Oi;/Ot;/Gy;/Zo>")
    add_link_options($<$<CONFIG:Debug>:/DEBUG>)
    # enable ltcg for relwithdebinfo, but ICF disabled to avoid weird debugger breakpoints
    add_link_options("$<$<CONFIG:RelWithDebInfo>:/DEBUG;>")
    add_link_options("$<$<CONFIG:Release>:/LTCG;/OPT:REF;/OPT:ICF>")
endfunction()

# There are a number of warnings that clang-cl will emit that are not relevant to *our* code
# but come from third-party libraries and using clang-cl on windows. We only add this when
# on Win32 and using clang-cl with MSVC frontend/env
function(disable_msvc_frontend_clang_compiler_warnings_all_targets)
    add_compile_definitions("_CRT_SECURE_NO_WARNINGS")
    # -Wno-unused-command-line-argument is needed as clang-cl will emit this warning
    # when using /MP and for the appended /Zc:preprocessor
    add_compile_options("-Wno-unused-command-line-argument")
endfunction()

# Specifies generator expressions for flags that are shared between Clang and Emscripten
# Makes list of different flags far more succinct
# RelWithDebInfo has to use -O1, as -O2 over-optimizes and -Og causes codegen 
# crashes with clang 22.1.8 when using the MSVC ABI/RT. this expands to 
# -fextend-variable-liveness=all, and that's the flag causing the crash
# todo-ship: scope -Og to just our code, rather than globally
function(add_shared_clang_style_build_flags_all_targets)
    add_compile_options(
        "$<$<CONFIG:Debug>:-O0;-glldb;>"
        "$<$<CONFIG:RelWithDebInfo>:-O1;-g;-flto>"
        "$<$<CONFIG:Release>:-O3;-flto;-fno-exceptions;-fno-rtti;-fno-threadsafe-statics>"
        "$<$<CONFIG:MinSizeRel>:-Oz;-flto;-fno-exceptions;-fno-rtti;-fno-threadsafe-statics>")
    add_link_options(
        "-fuse-ld=lld"
        "$<$<CONFIG:Debug>:-O0;-glldb>"
        "$<$<CONFIG:RelWithDebInfo>:-O1;-g;-flto>"
        "$<$<CONFIG:Release>:-O3>"
        "$<$<CONFIG:MinSizeRel>:-Oz;-flto>")
endfunction()

function(add_clang_style_build_flags_all_targets)
    add_compile_definitions(
        "$<$<CONFIG:RelWithDebInfo,Release>:_HAS_EXCEPTIONS=0>")
endfunction()
