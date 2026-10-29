cmake_minimum_required(VERSION 4.2)
if(NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang)
endif()
project(ocean-c C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

include_directories(.)

find_path(OCEAN_FFI_INCLUDE_DIR ffi.h REQUIRED)

add_executable(ocean-c)
target_include_directories(ocean-c PRIVATE "${OCEAN_FFI_INCLUDE_DIR}")

target_sources(ocean-c
    PRIVATE
        main.c
        lsp.c
        package.c
        dynarray.c
        c-hashmap/map.c
        token.c
        scanner.c
        parser.c
        ast.c
        semantic.c
        backend.c
)

if(UNIX)
    target_link_libraries(ocean-c PRIVATE m)
endif()

add_custom_target(ocean ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ocean-c>" "$<TARGET_FILE_DIR:ocean-c>/ocean"
    DEPENDS ocean-c)
install(PROGRAMS "$<TARGET_FILE:ocean-c>" DESTINATION bin RENAME ocean)

include(CTest)
if(BUILD_TESTING)
    set(ocean_test_targets function_parser_test parser_features_test parser_edges_test parser_extensions_test)
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
    add_executable(semantic_test tests/semantic_test.c semantic.c parser.c scanner.c token.c dynarray.c)
    list(APPEND ocean_test_targets semantic_test)
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

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME lsp_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/lsp_test.py" "$<TARGET_FILE:ocean-c>")
    set_tests_properties(lsp_test PROPERTIES TIMEOUT 30)
    add_test(NAME semantic_diagnostics_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/semantic_diagnostics_test.py" "$<TARGET_FILE:ocean-c>")
    set_tests_properties(semantic_diagnostics_test PROPERTIES TIMEOUT 30)
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

    find_program(OCEAN_CLANG_EXECUTABLE clang REQUIRED)
    add_test(NAME fallthrough_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fallthrough_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(fallthrough_test PROPERTIES TIMEOUT 60)
    add_test(NAME type_constraints_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/type_constraints_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(type_constraints_test PROPERTIES TIMEOUT 60)
    add_test(NAME backend_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(backend_test PROPERTIES TIMEOUT 60)
    add_test(NAME type_definitions_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/type_definitions_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(type_definitions_test PROPERTIES TIMEOUT 60)
    add_test(NAME mixed_arrays_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/mixed_arrays_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(mixed_arrays_test PROPERTIES TIMEOUT 60)
    add_test(NAME io_backend_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/io_backend_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(io_backend_test PROPERTIES TIMEOUT 60)
    add_test(NAME range_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/range_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(range_test PROPERTIES TIMEOUT 60)
    add_test(NAME c_variadic_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/c_variadic_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(c_variadic_test PROPERTIES TIMEOUT 60)
    add_test(NAME package_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_test.py" "$<TARGET_FILE:ocean-c>")
    set_tests_properties(package_test PROPERTIES TIMEOUT 90)
    add_test(NAME goto_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/goto_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(goto_test PROPERTIES TIMEOUT 60)
    add_test(NAME panic_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/panic_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(panic_test PROPERTIES TIMEOUT 60)
    add_test(NAME array_bounds_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/array_bounds_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(array_bounds_test PROPERTIES TIMEOUT 60)
    add_test(NAME constant_builtins_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/constant_builtins_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(constant_builtins_test PROPERTIES TIMEOUT 60)
    add_test(NAME array_addition_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/array_addition_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(array_addition_test PROPERTIES TIMEOUT 60)
    add_test(NAME string_dynamic_array_ownership_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/string_dynamic_array_ownership_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(string_dynamic_array_ownership_test PROPERTIES TIMEOUT 60)
    add_test(NAME new_allocation_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/new_allocation_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(new_allocation_test PROPERTIES TIMEOUT 60)
    add_test(NAME collection_types_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/collection_types_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(collection_types_test PROPERTIES TIMEOUT 60)
    add_test(NAME array_values_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/array_values_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(array_values_test PROPERTIES TIMEOUT 60)
endif()
