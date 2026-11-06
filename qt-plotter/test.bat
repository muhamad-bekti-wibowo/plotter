@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set QTDIR=C:\Qt\6.11.1\msvc2022_64
set PATH=C:\Qt\Tools\Ninja;C:\Qt\Tools\CMake_64\bin;%QTDIR%\bin;%PATH%
cd /d %~dp0
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=%QTDIR% >nul || exit /b 1
cmake --build build --target selftest || exit /b 1
set QT_QPA_PLATFORM=offscreen
build\selftest.exe
