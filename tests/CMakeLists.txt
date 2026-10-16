find_package(Qt6 REQUIRED COMPONENTS Test)

set(SEASHOT_TESTS tst_screenmapper tst_selection tst_settings)
if(XCB_FOUND)
    list(APPEND SEASHOT_TESTS tst_ewmh)
endif()

foreach(test_name ${SEASHOT_TESTS})
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE seashot_core Qt6::Test)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
