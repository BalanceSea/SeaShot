find_package(Qt6 REQUIRED COMPONENTS Test)

set(SEASHOT_TESTS tst_screenmapper tst_selection tst_settings tst_annotation tst_settingsdialog tst_tray
                 tst_gifencoder tst_gifrecorder tst_scrollstitcher tst_ocrengine tst_application
                 tst_theme tst_desktopservices)
find_program(DBUS_RUN_SESSION dbus-run-session)
if(DBUS_RUN_SESSION)
    list(APPEND SEASHOT_TESTS tst_screenshot2)
endif()
if(XCB_FOUND)
    list(APPEND SEASHOT_TESTS tst_ewmh)
endif()

foreach(test_name ${SEASHOT_TESTS})
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE seashot_core Qt6::Test)
    if(test_name STREQUAL "tst_screenshot2")
        add_test(NAME ${test_name} COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:${test_name}>)
    else()
        add_test(NAME ${test_name} COMMAND ${test_name})
    endif()
endforeach()

# 画布/配置界面测试要建 QWidget：用 offscreen 平台，避免测试时弹窗、也不依赖会话显示环境
set_tests_properties(tst_annotation tst_settingsdialog tst_tray tst_scrollstitcher tst_ocrengine tst_theme PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_tests_properties(tst_application PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SEASHOT_RECORD_POLLING=1;SEASHOT_NO_FIRST_RUN_ASK=1")
if(DBUS_RUN_SESSION)
    set_tests_properties(tst_screenshot2 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
target_compile_definitions(tst_application PRIVATE SEASHOT_BINARY="$<TARGET_FILE:seashot>")
add_dependencies(tst_application seashot)

# Test the shipped JavaScript policy with Node's built-in VM when available.
find_program(SEASHOT_NODE_EXECUTABLE NAMES node nodejs)
if(SEASHOT_NODE_EXECUTABLE)
    add_test(NAME tst_kwinscript COMMAND ${SEASHOT_NODE_EXECUTABLE}
             ${CMAKE_CURRENT_SOURCE_DIR}/tst_kwinscript.js
             ${CMAKE_SOURCE_DIR}/resources/seashot-kwin.js)
endif()
