cmake_minimum_required(VERSION 4.2)
if(NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang)
endif()
project(ocean-c C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

include_directories(.)

add_executable(ocean-c)

target_sources(ocean-c
    PRIVATE
        main.c
        dynarray.c
        c-hashmap/map.c
        token.c
        scanner.c
        parser.c
        ast.c
)

if(UNIX)
    target_link_libraries(ocean-c PRIVATE m)
endif()

include(CTest)
if(BUILD_TESTING)
    set(ocean_test_targets function_parser_test parser_features_test parser_edges_test)
    foreach(test_name IN LISTS ocean_test_targets)
        # Parser suites include parser.c to inspect internal error paths.
        add_executable(${test_name} tests/${test_name}.c scanner.c token.c dynarray.c)
    endforeach()
    if(UNIX)
        # These suites isolate exiting/aborting APIs with POSIX subprocesses.
        foreach(test_name scanner_test token_test dynarray_test)
            add_executable(${test_name} tests/${test_name}.c scanner.c token.c dynarray.c)
            list(APPEND ocean_test_targets ${test_name})
        endforeach()
        add_executable(ast_test tests/ast_test.c ast.c)
        list(APPEND ocean_test_targets ast_test)
    endif()
    add_executable(hashmap_test tests/hashmap_test.c c-hashmap/map.c)
    list(APPEND ocean_test_targets hashmap_test)
    foreach(test_name IN LISTS ocean_test_targets)
        if(UNIX)
            target_link_libraries(${test_name} PRIVATE m)
        endif()
        target_compile_options(${test_name} PRIVATE -UNDEBUG)
        add_test(NAME ${test_name} COMMAND ${test_name})
        set_tests_properties(${test_name} PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)
    endforeach()
    add_test(NAME cli_test COMMAND ${CMAKE_COMMAND}
        "-DOCEAN_EXE=$<TARGET_FILE:ocean-c>"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-fixtures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_test.cmake")
    set_tests_properties(cli_test PROPERTIES TIMEOUT 30)

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(pending_probe tests/pending_probe.c parser.c scanner.c token.c dynarray.c ast.c)
    target_compile_options(pending_probe PRIVATE -UNDEBUG)
    if(UNIX)
        target_link_libraries(pending_probe PRIVATE m)
    endif()
    set(pending_manifest "${CMAKE_CURRENT_SOURCE_DIR}/tests/pending/features.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${pending_manifest}")
    execute_process(COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/pending_features.py" --list
        RESULT_VARIABLE pending_status OUTPUT_VARIABLE pending_cases
        ERROR_VARIABLE pending_error OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT pending_status EQUAL 0)
        message(FATAL_ERROR "Invalid pending feature inventory: ${pending_error}")
    endif()
    string(REPLACE "\n" ";" pending_cases "${pending_cases}")
    foreach(pending_case IN LISTS pending_cases)
        add_test(NAME "pending.${pending_case}" COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/pending_features.py"
            --probe "$<TARGET_FILE:pending_probe>" --case "${pending_case}")
        set_tests_properties("pending.${pending_case}" PROPERTIES
            SKIP_RETURN_CODE 77 LABELS "pending" TIMEOUT 30)
    endforeach()
    add_test(NAME pending_inventory COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/pending_features.py" --validate)
    add_test(NAME pending_runner_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/pending_runner_test.py")
    set_tests_properties(pending_inventory pending_runner_test PROPERTIES TIMEOUT 30)
endif()
