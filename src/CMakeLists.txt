# SeaShot 核心库（供可执行文件与单元测试共用）
set(SEASHOT_CORE_SOURCES
    core/SelectionModel.cpp
    geometry/ScreenMapper.cpp
    util/Clipboard.cpp
    backend/BackendFactory.cpp
    backend/wayland/KWinBridge.cpp
    backend/wayland/ScreenShot2.cpp
    backend/wayland/WaylandBackend.cpp
    overlay/Overlay.cpp
    ui/Theme.cpp
    editor/Annotation.cpp
    editor/AnnotationDocument.cpp
    editor/AnnotationRenderer.cpp
    editor/AnnotationCanvas.cpp
    editor/EditorWindow.cpp
    scroll/ScrollStitcher.cpp
    record/GifEncoder.cpp
    record/GifRecorder.cpp
    record/RecordingIndicator.cpp
    record/PortalScreencast.cpp
    app/Settings.cpp
    app/SettingsDialog.cpp
    app/SelfTest.cpp
    app/PinWindow.cpp
    app/Toast.cpp
    app/Tray.cpp
    app/Application.cpp
)

set(SEASHOT_HEADERS
    core/Types.h
    core/SelectionModel.h
    geometry/ScreenMapper.h
    util/Clipboard.h
    util/Log.h
    backend/CaptureBackend.h
    backend/BackendFactory.h
    backend/wayland/KWinBridge.h
    backend/wayland/ScreenShot2.h
    backend/wayland/WaylandBackend.h
    overlay/Overlay.h
    ui/Theme.h
    editor/Annotation.h
    editor/AnnotationDocument.h
    editor/AnnotationRenderer.h
    editor/AnnotationCanvas.h
    editor/EditorWindow.h
    scroll/ScrollStitcher.h
    record/GifEncoder.h
    record/GifRecorder.h
    record/RecordingIndicator.h
    record/PortalScreencast.h
    app/Settings.h
    app/SettingsDialog.h
    app/SelfTest.h
    app/PinWindow.h
    app/Toast.h
    app/Tray.h
    app/Application.h
)

if(XCB_FOUND)
    list(APPEND SEASHOT_CORE_SOURCES backend/x11/Ewmh.cpp backend/x11/X11Backend.cpp)
    list(APPEND SEASHOT_HEADERS backend/x11/Ewmh.h backend/x11/X11Backend.h)
endif()

if(PIPEWIRE_FOUND)
    list(APPEND SEASHOT_CORE_SOURCES record/PipeWireFrameSource.cpp)
    list(APPEND SEASHOT_HEADERS record/PipeWireFrameSource.h)
else()
    # 依赖缺失时也要能编译：让 PipeWireFrameSource 以"不可用"形态存在，
    # 上层据此回退到 ScreenShot2 轮询（避免到处写 #ifdef）
    list(APPEND SEASHOT_CORE_SOURCES record/PipeWireFrameSource.cpp)
    list(APPEND SEASHOT_HEADERS record/PipeWireFrameSource.h)
endif()

add_library(seashot_core STATIC ${SEASHOT_CORE_SOURCES} ${SEASHOT_HEADERS})
target_include_directories(seashot_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(seashot_core PUBLIC
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::DBus Qt6::Network
    KF6::GlobalAccel KF6::StatusNotifierItem
    LayerShellQt::Interface
)
if(PIPEWIRE_FOUND)
    target_link_libraries(seashot_core PUBLIC PkgConfig::PIPEWIRE)
    target_compile_definitions(seashot_core PUBLIC SEASHOT_HAVE_PIPEWIRE=1)
endif()

if(XCB_FOUND)
    target_link_libraries(seashot_core PUBLIC PkgConfig::XCB)
    target_compile_definitions(seashot_core PUBLIC SEASHOT_WITH_X11=1)
endif()

# KWin 助手脚本随二进制内嵌，运行时释放到缓存目录供 KWin 加载
qt_add_resources(seashot_core "kwinscript"
    PREFIX "/seashot"
    BASE "${CMAKE_SOURCE_DIR}"
    FILES "${CMAKE_SOURCE_DIR}/resources/seashot-kwin.js"
          "${CMAKE_SOURCE_DIR}/resources/icons/seashot.png"
)

add_executable(seashot main.cpp)
target_link_libraries(seashot PRIVATE seashot_core)
