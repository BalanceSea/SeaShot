find_package(Qt6 REQUIRED COMPONENTS Test)

set(SEASHOT_TESTS tst_screenmapper tst_selection tst_settings tst_annotation tst_settingsdialog tst_tray
                 tst_gifencoder tst_gifrecorder)
if(XCB_FOUND)
    list(APPEND SEASHOT_TESTS tst_ewmh)
endif()

foreach(test_name ${SEASHOT_TESTS})
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE seashot_core Qt6::Test)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# 画布/配置界面测试要建 QWidget：用 offscreen 平台，避免测试时弹窗、也不依赖会话显示环境
set_tests_properties(tst_annotation tst_settingsdialog tst_tray PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
