# Phase 0 探测程序的构建（同时用来验证 CMake + Qt6 + LayerShellQt 的可用性，
# 这套 find_package 写法将直接用于 SeaShot 主工程的 CMakeLists.txt）
cmake_minimum_required(VERSION 3.21)
project(seashot-probes LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets DBus)
find_package(LayerShellQt REQUIRED)

add_executable(shot2probe ../shot2probe.cpp)
target_link_libraries(shot2probe PRIVATE Qt6::Core Qt6::Gui Qt6::DBus)

add_executable(testwindow ../testwindow.cpp)
target_link_libraries(testwindow PRIVATE Qt6::Widgets)

add_executable(overlayprobe ../overlayprobe.cpp)
target_link_libraries(overlayprobe PRIVATE Qt6::Widgets LayerShellQt::Interface)

add_executable(inplace-probe ../inplace-probe.cpp)
target_link_libraries(inplace-probe PRIVATE Qt6::Widgets LayerShellQt::Interface)

# Phase D 可行性探针：滚动截屏的重叠区匹配拼接（纯 QImage 计算，不需要 layer-shell）
add_executable(scroll-stitch-probe ../scroll-stitch-probe.cpp)
target_link_libraries(scroll-stitch-probe PRIVATE Qt6::Gui)
