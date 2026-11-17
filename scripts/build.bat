@echo off
REM Builds Lodestone. Usage: scripts\build.bat [Debug|RelWithDebInfo] [preset]
REM
REM The build tree keeps the compiler that CMakeCache.txt holds, which is Visual Studio 18
REM Community. Another environment gives that compiler the headers of a different toolset,
REM ammintrin.h raises C4392, and every SPIRV-Tools target fails because Slang builds them
REM with /WX. This script therefore sets the environment itself.
setlocal

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Debug"
set "PRESET=%~2"
if "%PRESET%"=="" set "PRESET=ninja-clang-cl"

set "REPO=%~dp0.."
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%VCVARS%" (
    echo [build] cannot find the Visual Studio 18 Community environment at:
    echo [build]   %VCVARS%
    echo [build] Edit VCVARS in this script if the install moved.
    exit /b 1
)

REM vcvars64.bat calls vswhere.exe, which is not on PATH by default.
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "%VCVARS%" >nul

cmake --build "%REPO%\build\%PRESET%" --config %CONFIG%
set "RESULT=%ERRORLEVEL%"
if not "%RESULT%"=="0" echo [build] FAILED with exit code %RESULT%
exit /b %RESULT%
