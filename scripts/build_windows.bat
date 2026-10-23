@echo off
rem Builds Ymir's SDL3 frontend with the Drydock runtime (game libraries,
rem ship mode). Run from "x64 Native Tools Command Prompt for VS".
rem
rem   scripts\build_windows.bat [path\to\generated-dir]
rem
rem With a directory of sh2recomp output, that game is linked into the
rem executable (development builds); without it, games are loaded at run time
rem from recomp\<product>.dll.

setlocal
set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "YMIR=%ROOT%\external\Ymir"
set "GEN=%~1"
if not "%GEN%"=="" for %%I in ("%GEN%") do set "GEN=%%~fI"
set "BUILD=build"

if not exist "%YMIR%\CMakeLists.txt" (
    echo Ymir not found in %YMIR%. Clone it first:
    echo   git submodule update --init --recursive
    exit /b 1
)
if not "%GEN%"=="" if not exist "%GEN%\*.cpp" (
    echo No generated .cpp files in %GEN%
    exit /b 1
)
where clang-cl >nul 2>nul || (
    echo clang-cl not found. Install "C++ Clang Compiler for Windows" in the Visual Studio Installer
    echo and run this script from "x64 Native Tools Command Prompt for VS".
    exit /b 1
)

rem CMake prefers forward slashes
set "ROOT_F=%ROOT:\=/%"
rem every generated .cpp as a CMake list
set "GEN_F="
setlocal EnableDelayedExpansion
if not "%GEN%"=="" for %%F in ("%GEN%\*.cpp") do (
    set "F=%%~fF"
    if defined GEN_F (set "GEN_F=!GEN_F!;!F:\=/!") else (set "GEN_F=!F:\=/!")
)
endlocal & set "GEN_F=%GEN_F%"

if not exist "%YMIR%\vcpkg\vcpkg.exe" (
    echo === Bootstrapping vcpkg
    call "%YMIR%\vcpkg\bootstrap-vcpkg.bat" -disableMetrics || exit /b 1
)

pushd "%YMIR%"
echo === Configuring (the first time vcpkg builds SDL3, curl, etc.: this takes a while)
rem Ymir's presets set toolset=ClangCL, which only Visual Studio itself accepts with Ninja.
cmake -S . -B %BUILD% -G Ninja ^
    -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake ^
    -DVCPKG_TARGET_TRIPLET=x64-win-llvm-avx2-static-md -DBUILD_SHARED_LIBS=OFF -DYmir_AVX2=ON ^
    -DYmir_ENABLE_TESTS=OFF -DYmir_ENABLE_SANDBOX=OFF -DYmir_ENABLE_YMDASM=OFF ^
    -DYmir_ENABLE_DEVLOG=OFF -DYmir_ENABLE_DEV_ASSERTIONS=OFF -DYmir_ENABLE_UPDATE_CHECKS=OFF ^
    "-DYMIR_RECOMP_ROOT=%ROOT_F%" "-DYMIR_RECOMP_GENERATED=%GEN_F%" || (popd & exit /b 1)

echo === Building ymir-sdl3 (the generated code alone takes a few minutes)
cmake --build %BUILD% --target ymir-sdl3 || (popd & exit /b 1)
popd

echo.
echo Done. Run:
echo   "%YMIR%\%BUILD%\apps\ymir-sdl3\ymir-sdl3.exe"
