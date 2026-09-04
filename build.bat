@echo off
setlocal
where cmake >nul 2>nul || (echo CMake 3.25+ not found.& exit /b 1)
if "%VCPKG_ROOT%"=="" (echo Set VCPKG_ROOT to your vcpkg directory.& exit /b 1)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" -DEVIDENCE_BUILD_TESTS=ON || exit /b 1
cmake --build build --config Release --parallel || exit /b 1
ctest --test-dir build -C Release --output-on-failure || exit /b 1
echo.
echo Built: build\bin\Release\GambitRecord.exe
endlocal
