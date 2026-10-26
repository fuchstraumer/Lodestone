@echo off
REM Runs every test executable and prints one line for each. Usage: scripts\run-tests.bat [Debug|RelWithDebInfo] [preset]
REM
REM The configuration defaults to RelWithDebInfo. The old Debug binaries were built by an older Slang. They
REM write module files that the current Slang rejects, into the shared module cache folder. CLAUDE.md
REM ("Build and test") has the measurement.
REM
REM Each test runs directly, and not through ctest. A direct run prints as it goes. A run behind a
REM pager or behind PowerShell `Select-Object` prints nothing until the end, which looks like a
REM deadlock, and a test that stops with an assertion loses its buffered output.
REM
REM Each cook test takes a command line. With no argument it exits 1 on NoOutputSpecified, which reads
REM like a failure. tests/CMakeLists.txt gives ctest the same arguments this script gives it.
REM
REM Each check compares the exit code with 0. A crash exits with a negative code, such as 0xC0000409
REM from std::terminate, and `if errorlevel 1` is false for a negative code. That check once reported
REM crashing cook tests as passes.
setlocal enabledelayedexpansion

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=RelWithDebInfo"
set "PRESET=%~2"
if "%PRESET%"=="" set "PRESET=ninja-msvc"

set "REPO=%~dp0.."
set "BIN=%REPO%\build\%PRESET%\tests\%CONFIG%"

if not exist "%BIN%" (
    echo [tests] no test binaries at %BIN%. Run scripts\build.bat first.
    exit /b 1
)

set "FAILED=0"

for %%T in ("%BIN%\*Test.exe") do (
    if /I not "%%~nT"=="EntryPointParamsCookTest" if /I not "%%~nT"=="ParameterBlocksCookTest" if /I not "%%~nT"=="WorkgroupMemoryCookTest" if /I not "%%~nT"=="CrossModuleCookTest" if /I not "%%~nT"=="InterfaceAxisCookTest" if /I not "%%~nT"=="KitchenSinkCookTest" if /I not "%%~nT"=="KitchenSinkOptimizedCookTest" (
        "%%~fT" >nul 2>&1
        if !ERRORLEVEL! neq 0 (
            echo [FAIL] %%~nT
            set "FAILED=1"
            "%%~fT"
        ) else (
            echo [ ok ] %%~nT
        )
    )
)

REM The interface-axis end-to-end cook (phase E step E7). It cooks six variants over a Type axis
REM crossed with a boolean axis.
"%BIN%\InterfaceAxisCookTest.exe" -o "%REPO%\build\%PRESET%\tests\interface_axis_output" --target=wgsl --target=spirv --verify-deterministic "%REPO%\tests\assets\InterfaceAxis\InterfaceAxisTest.slang" >nul 2>&1
if !ERRORLEVEL! neq 0 (
    echo [FAIL] InterfaceAxisCookTest
    set "FAILED=1"
) else (
    echo [ ok ] InterfaceAxisCookTest
)

REM The same driver, on the probe module for the entry point parameter scope. It cooks one variant.
"%BIN%\EntryPointParamsCookTest.exe" -o "%REPO%\build\%PRESET%\tests\entry_point_params_output" --target=wgsl --target=spirv --verify-deterministic "%REPO%\tests\assets\EntryPointParams.slang" >nul 2>&1
if !ERRORLEVEL! neq 0 (
    echo [FAIL] EntryPointParamsCookTest
    set "FAILED=1"
) else (
    echo [ ok ] EntryPointParamsCookTest
)

REM The same driver, on the probe module for the parameter block walk. It cooks one variant.
"%BIN%\ParameterBlocksCookTest.exe" -o "%REPO%\build\%PRESET%\tests\parameter_blocks_output" --target=wgsl --target=spirv --verify-deterministic "%REPO%\tests\assets\ParameterBlocks.slang" >nul 2>&1
if !ERRORLEVEL! neq 0 (
    echo [FAIL] ParameterBlocksCookTest
    set "FAILED=1"
) else (
    echo [ ok ] ParameterBlocksCookTest
)

REM Two modules in separate folders, cooked in one run. The kernel imports the other module through --include-dir.
"%BIN%\CrossModuleCookTest.exe" -o "%REPO%\build\%PRESET%\tests\cross_module_output" --target=spirv --include-dir "%REPO%\tests\assets\CrossModule\lib" --verify-deterministic "%REPO%\tests\assets\CrossModule\lib\CmShared.slang" "%REPO%\tests\assets\CrossModule\app\CmKernel.slang" >nul 2>&1
if !ERRORLEVEL! neq 0 (
    echo [FAIL] CrossModuleCookTest
    set "FAILED=1"
) else (
    echo [ ok ] CrossModuleCookTest
)

REM The workgroup memory probe. The cross-profile validator compares the bytes and the 16-bit float requirement.
"%BIN%\WorkgroupMemoryCookTest.exe" -o "%REPO%\build\%PRESET%\tests\workgroup_memory_output" --target=wgsl --target=spirv --verify-deterministic "%REPO%\tests\assets\WorkgroupMemory.slang" >nul 2>&1
if !ERRORLEVEL! neq 0 (
    echo [FAIL] WorkgroupMemoryCookTest
    set "FAILED=1"
) else (
    echo [ ok ] WorkgroupMemoryCookTest
)

REM The multi-module KitchenSink cook. Four consumer modules cook together against one policy, for
REM both targets. It is the stress and coverage asset. Exit code 0 states that every variant compiled,
REM every reflection agreed with the emitted WGSL and SPIR-V, every round trip read back, and two cooks
REM agreed byte for byte.
"%BIN%\KitchenSinkCookTest.exe" -o "%REPO%\build\%PRESET%\tests\kitchen_sink_output" --target=wgsl --target=spirv --verify-deterministic --policy-file "%REPO%\tests\assets\KitchenSink\KitchenSink.toml" "%REPO%\tests\assets\KitchenSink\KsGeometry.slang" "%REPO%\tests\assets\KitchenSink\KsMaterial.slang" "%REPO%\tests\assets\KitchenSink\KsVolume.slang" "%REPO%\tests\assets\KitchenSink\KsPost.slang" >nul 2>&1
if !ERRORLEVEL! neq 0 (
    echo [FAIL] KitchenSinkCookTest
    set "FAILED=1"
) else (
    echo [ ok ] KitchenSinkCookTest
)

REM The same KitchenSink cook at --O2 --g3. The measurements read the base form, so the inert axis policy still holds.
"%BIN%\KitchenSinkOptimizedCookTest.exe" -o "%REPO%\build\%PRESET%\tests\kitchen_sink_optimized_output" --target=wgsl --target=spirv --verify-deterministic --O2 --g3 --policy-file "%REPO%\tests\assets\KitchenSink\KitchenSink.toml" "%REPO%\tests\assets\KitchenSink\KsGeometry.slang" "%REPO%\tests\assets\KitchenSink\KsMaterial.slang" "%REPO%\tests\assets\KitchenSink\KsVolume.slang" "%REPO%\tests\assets\KitchenSink\KsPost.slang" >nul 2>&1
if !ERRORLEVEL! neq 0 (
    echo [FAIL] KitchenSinkOptimizedCookTest
    set "FAILED=1"
) else (
    echo [ ok ] KitchenSinkOptimizedCookTest
)

if "%FAILED%"=="1" (
    echo [tests] at least one target failed
    exit /b 1
)

echo [tests] all targets passed
exit /b 0
