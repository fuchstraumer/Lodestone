@echo off
REM Runs every test executable and prints one line for each. Usage: scripts\run-tests.bat [Debug|RelWithDebInfo] [preset]
REM
REM Each test runs directly, and not through ctest. A direct run prints as it goes. A run behind a
REM pager or behind PowerShell `Select-Object` prints nothing until the end, which looks like a
REM deadlock, and a test that stops with an assertion loses its buffered output.
REM
REM Each cook test takes a command line. With no argument it exits 1 on NoOutputSpecified, which reads
REM like a failure. tests/CMakeLists.txt gives ctest the same arguments this script gives it.
setlocal enabledelayedexpansion

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Debug"
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
    if /I not "%%~nT"=="EntryPointParamsCookTest" if /I not "%%~nT"=="ParameterBlocksCookTest" if /I not "%%~nT"=="InterfaceAxisCookTest" if /I not "%%~nT"=="EnumAxisCookTest" if /I not "%%~nT"=="KitchenSinkCookTest" (
        "%%~fT" >nul 2>&1
        if errorlevel 1 (
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
"%BIN%\InterfaceAxisCookTest.exe" -o "%REPO%\build\%PRESET%\tests\interface_axis_output" --target=wgsl --verify-deterministic "%REPO%\tests\assets\InterfaceAxis\InterfaceAxisTest.slang" >nul 2>&1
if errorlevel 1 (
    echo [FAIL] InterfaceAxisCookTest
    set "FAILED=1"
) else (
    echo [ ok ] InterfaceAxisCookTest
)

REM The enum-axis end-to-end cook. It cooks six variants over an Enum axis (three cases addressed by
REM name, with non-ascending underlying values) crossed with a boolean axis.
"%BIN%\EnumAxisCookTest.exe" -o "%REPO%\build\%PRESET%\tests\enum_axis_output" --target=wgsl --verify-deterministic "%REPO%\tests\assets\EnumAxis\EnumAxisTest.slang" >nul 2>&1
if errorlevel 1 (
    echo [FAIL] EnumAxisCookTest
    set "FAILED=1"
) else (
    echo [ ok ] EnumAxisCookTest
)

REM The same driver, on the probe module for the entry point parameter scope. It cooks one variant.
"%BIN%\EntryPointParamsCookTest.exe" -o "%REPO%\build\%PRESET%\tests\entry_point_params_output" --target=wgsl --verify-deterministic "%REPO%\tests\assets\EntryPointParams.slang" >nul 2>&1
if errorlevel 1 (
    echo [FAIL] EntryPointParamsCookTest
    set "FAILED=1"
) else (
    echo [ ok ] EntryPointParamsCookTest
)

REM The same driver, on the probe module for the parameter block walk. It cooks one variant.
"%BIN%\ParameterBlocksCookTest.exe" -o "%REPO%\build\%PRESET%\tests\parameter_blocks_output" --target=wgsl --verify-deterministic "%REPO%\tests\assets\ParameterBlocks.slang" >nul 2>&1
if errorlevel 1 (
    echo [FAIL] ParameterBlocksCookTest
    set "FAILED=1"
) else (
    echo [ ok ] ParameterBlocksCookTest
)

REM The multi-module KitchenSink cook. Four consumer modules cook together against one policy. It is
REM the stress and coverage asset. Exit code 0 states that every variant compiled, every reflection
REM agreed with the emitted WGSL, every round trip read back, and two cooks agreed byte for byte.
"%BIN%\KitchenSinkCookTest.exe" -o "%REPO%\build\%PRESET%\tests\kitchen_sink_output" --target=wgsl --verify-deterministic --policy-file "%REPO%\tests\assets\KitchenSink\KitchenSink.toml" "%REPO%\tests\assets\KitchenSink\KsGeometry.slang" "%REPO%\tests\assets\KitchenSink\KsMaterial.slang" "%REPO%\tests\assets\KitchenSink\KsVolume.slang" "%REPO%\tests\assets\KitchenSink\KsPost.slang" >nul 2>&1
if errorlevel 1 (
    echo [FAIL] KitchenSinkCookTest
    set "FAILED=1"
) else (
    echo [ ok ] KitchenSinkCookTest
)

if "%FAILED%"=="1" (
    echo [tests] at least one target failed
    exit /b 1
)

echo [tests] all targets passed
exit /b 0
