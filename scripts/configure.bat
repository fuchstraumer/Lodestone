@echo off
REM Configures a build tree. Usage: scripts\configure.bat [preset]
REM
REM Run this after a change to a CMakeLists.txt that adds or removes a target. It sets the same
REM compiler environment scripts\build.bat sets. A configure from another environment writes a cache
REM that names a different toolset, and every SPIRV-Tools target then fails.
setlocal

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=ninja-clang-cl"

set "REPO=%~dp0.."
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%VCVARS%" (
    echo [configure] cannot find the Visual Studio 18 Community environment at:
    echo [configure]   %VCVARS%
    echo [configure] Edit VCVARS in this script if the install moved.
    exit /b 1
)

REM vcvars64.bat calls vswhere.exe, which is not on PATH by default.
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "%VCVARS%" >nul

pushd "%REPO%"
cmake --preset %PRESET%
set "RESULT=%ERRORLEVEL%"
popd
if not "%RESULT%"=="0" echo [configure] FAILED with exit code %RESULT%
exit /b %RESULT%
