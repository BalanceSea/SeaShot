cmake_minimum_required(VERSION 3.21)
project(SeaShot VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets DBus Network Test)
# 本机只装了 KF6 的独立包（没有 KF6 伞包 KF6Config.cmake），因此逐个查找
find_package(KF6GlobalAccel REQUIRED)
find_package(KF6StatusNotifierItem REQUIRED)
find_package(LayerShellQt REQUIRED)
find_package(PkgConfig REQUIRED)

# X11 后端：xcb + xcb-ewmh。缺失时只关掉 X11 后端，不让整体构建失败。
pkg_check_modules(XCB IMPORTED_TARGET xcb xcb-shm xcb-ewmh)
if(XCB_FOUND)
    message(STATUS "SeaShot: X11 后端启用（xcb ${XCB_xcb_VERSION}）")
else()
    message(WARNING "SeaShot: 未找到 xcb/xcb-ewmh，X11 后端将被编译关闭")
endif()

add_subdirectory(src)
enable_testing()
add_subdirectory(tests)
