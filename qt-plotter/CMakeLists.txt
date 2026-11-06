cmake_minimum_required(VERSION 3.21)
project(QtPlotter LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
find_package(Qt6 QUIET OPTIONAL_COMPONENTS SerialPort)   # tanpa SerialPort: Windows memakai fallback Win32 API

qt_add_executable(QtPlotter WIN32
    src/main.cpp src/MainWindow.cpp src/MainWindow.h
    src/Transport.cpp src/Transport.h
    src/PlotterLink.cpp src/PlotterLink.h
    src/SvgImport.cpp src/SvgImport.h src/GcodeImport.cpp src/GcodeImport.h
    src/ImageTracer.cpp src/ImageTracer.h
    src/PathPlanner.cpp src/PathPlanner.h
    src/CanvasView.cpp src/CanvasView.h
    src/Types.h)
target_link_libraries(QtPlotter PRIVATE Qt6::Widgets Qt6::Network)
target_compile_definitions(QtPlotter PRIVATE _USE_MATH_DEFINES)
if(TARGET Qt6::SerialPort)
    target_link_libraries(QtPlotter PRIVATE Qt6::SerialPort)
    target_compile_definitions(QtPlotter PRIVATE HAVE_QTSERIALPORT)
endif()

# Uji logika tanpa hardware: cmake --build build --target selftest && build\selftest.exe
qt_add_executable(selftest tests/selftest.cpp src/GcodeImport.cpp src/SvgImport.cpp src/ImageTracer.cpp src/PathPlanner.cpp)
target_link_libraries(selftest PRIVATE Qt6::Gui)
target_compile_definitions(selftest PRIVATE _USE_MATH_DEFINES)
