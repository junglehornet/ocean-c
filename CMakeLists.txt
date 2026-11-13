cmake_minimum_required(VERSION 4.2)
if(NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang)
endif()
project(ocean-c C)

set(OCEAN_VERSION "dev" CACHE STRING "Ocean compiler version string")
if(DEFINED ENV{VERSION})
    set(OCEAN_VERSION "$ENV{VERSION}" CACHE STRING "Ocean compiler version string" FORCE)
endif()
foreach(field IN ITEMS COMMIT COMMIT_DATE COMMIT_YEAR BUILD_DATE)
    set(OCEAN_${field} "" CACHE STRING "Override compiler ${field} metadata (empty uses Git/build time)")
    if(DEFINED ENV{${field}})
        set(OCEAN_${field} "$ENV{${field}}" CACHE STRING "Override compiler ${field} metadata" FORCE)
    endif()
endforeach()
add_custom_target(ocean_build_info
    COMMAND ${CMAKE_COMMAND} "-DOCEAN_VERSION=${OCEAN_VERSION}"
        "-DOCEAN_COMMIT=${OCEAN_COMMIT}" "-DOCEAN_COMMIT_DATE=${OCEAN_COMMIT_DATE}"
        "-DOCEAN_COMMIT_YEAR=${OCEAN_COMMIT_YEAR}" "-DOCEAN_BUILD_DATE=${OCEAN_BUILD_DATE}"
        "-DOCEAN_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DOCEAN_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildInfo.cmake"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/ocean_build_info.h"
    VERBATIM)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

include_directories(.)

find_path(OCEAN_FFI_INCLUDE_DIR ffi.h REQUIRED)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/runtime_threads.c")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/runtime_threads.c" OCEAN_THREAD_RUNTIME_SOURCE)
string(REPLACE "\\" "\\\\" OCEAN_THREAD_RUNTIME_SOURCE "${OCEAN_THREAD_RUNTIME_SOURCE}")
string(REPLACE "\"" "\\\"" OCEAN_THREAD_RUNTIME_SOURCE "${OCEAN_THREAD_RUNTIME_SOURCE}")
string(REPLACE "\n" "\\n\"\n\"" OCEAN_THREAD_RUNTIME_SOURCE "${OCEAN_THREAD_RUNTIME_SOURCE}")
configure_file(runtime_threads_source.h.in runtime_threads_source.h @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/runtime_process.c")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/runtime_process.c" OCEAN_PROCESS_RUNTIME_SOURCE)
string(REPLACE "\\" "\\\\" OCEAN_PROCESS_RUNTIME_SOURCE "${OCEAN_PROCESS_RUNTIME_SOURCE}")
string(REPLACE "\"" "\\\"" OCEAN_PROCESS_RUNTIME_SOURCE "${OCEAN_PROCESS_RUNTIME_SOURCE}")
string(REPLACE "\n" "\\n\"\n\"" OCEAN_PROCESS_RUNTIME_SOURCE "${OCEAN_PROCESS_RUNTIME_SOURCE}")
configure_file(runtime_process_source.h.in runtime_process_source.h @ONLY)
add_executable(ocean-c)
add_dependencies(ocean-c ocean_build_info)
target_include_directories(ocean-c PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(ocean-c PRIVATE "${OCEAN_FFI_INCLUDE_DIR}")

target_sources(ocean-c
    PRIVATE
        main.c
        platform.c
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
        build_cache.c
        llvm_packages.c
)


if(UNIX)
    target_link_libraries(ocean-c PRIVATE m)
endif()

add_custom_target(ocean ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ocean-c>" "$<TARGET_FILE_DIR:ocean-c>/ocean${CMAKE_EXECUTABLE_SUFFIX}"
    DEPENDS ocean-c)
install(PROGRAMS "$<TARGET_FILE:ocean-c>" DESTINATION bin RENAME "ocean${CMAKE_EXECUTABLE_SUFFIX}")
set(OCEAN_STANDARD_LIBRARY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../ocean-src" CACHE PATH "Ocean standard library source tree")
install(DIRECTORY "${OCEAN_STANDARD_LIBRARY_SOURCE}/" DESTINATION src OPTIONAL
    PATTERN ".git" EXCLUDE PATTERN ".gitignore" EXCLUDE)

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
    add_test(NAME version_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/version_test.py" "$<TARGET_FILE:ocean-c>")
    add_test(NAME release_package_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_package_test.py")
    add_test(NAME installer_test COMMAND $<TARGET_FILE:ocean-c> test
        "${CMAKE_CURRENT_SOURCE_DIR}/installer" --notty)
    set_tests_properties(installer_test PROPERTIES TIMEOUT 120
        ENVIRONMENT "OCEAN_SRC=${OCEAN_STANDARD_LIBRARY_SOURCE}")
    if(UNIX)
        add_test(NAME ocean_test_tui_test COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ocean_test_tui_test.py" "$<TARGET_FILE:ocean-c>")
        set_tests_properties(ocean_test_tui_test PROPERTIES TIMEOUT 120
            ENVIRONMENT "OCEAN_SRC=${OCEAN_STANDARD_LIBRARY_SOURCE}")
    endif()
    if(UNIX)
        add_test(NAME ocean_test_packages_test COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ocean_test_packages_test.py" "$<TARGET_FILE:ocean-c>")
        set_tests_properties(ocean_test_packages_test PROPERTIES TIMEOUT 120
            ENVIRONMENT "OCEAN_SRC=${OCEAN_STANDARD_LIBRARY_SOURCE}")
    endif()
    add_test(NAME ocean_test_parallel_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ocean_test_parallel_test.py" "$<TARGET_FILE:ocean-c>")
    set_tests_properties(ocean_test_parallel_test PROPERTIES TIMEOUT 120
        ENVIRONMENT "OCEAN_SRC=${OCEAN_STANDARD_LIBRARY_SOURCE}")
    add_test(NAME ocean_test_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ocean_test_test.py" "$<TARGET_FILE:ocean-c>")
    set_tests_properties(ocean_test_test PROPERTIES TIMEOUT 120
        ENVIRONMENT "OCEAN_SRC=${OCEAN_STANDARD_LIBRARY_SOURCE}")
    add_test(NAME check_backend_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_backend_test.py" "$<TARGET_FILE:ocean-c>")
    set_tests_properties(check_backend_test PROPERTIES TIMEOUT 120)
    add_test(NAME lsp_semantic_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/lsp_semantic_test.py" "$<TARGET_FILE:ocean-c>")
    set_tests_properties(lsp_semantic_test PROPERTIES TIMEOUT 120)
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
    add_test(NAME result_parse_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/result_parse_test.py"
        "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(result_parse_test PROPERTIES TIMEOUT 120)
    add_test(NAME recoverable_io_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/recoverable_io_test.py"
        "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(recoverable_io_test PROPERTIES TIMEOUT 180)


    add_executable(asm_backend_probe platform.c c-hashmap/map.c tests/asm_backend_probe.c ast.c dynarray.c
        token.c scanner.c parser.c build_cache.c llvm_packages.c)
    target_include_directories(asm_backend_probe PRIVATE "${OCEAN_FFI_INCLUDE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
    if(UNIX)
        target_link_libraries(asm_backend_probe PRIVATE m)
    endif()
    add_test(NAME libc_symbol_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/libc_symbol_test.py"
        "$<TARGET_FILE:asm_backend_probe>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(libc_symbol_test PROPERTIES TIMEOUT 60)
    add_test(NAME standard_library_asm_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/standard_library_asm_test.py"
        "$<TARGET_FILE:ocean-c>" "$<TARGET_FILE:asm_backend_probe>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(standard_library_asm_test PROPERTIES TIMEOUT 120)

    add_test(NAME fallthrough_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fallthrough_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(fallthrough_test PROPERTIES TIMEOUT 60)
    add_test(NAME type_lists_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/type_lists_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(type_lists_test PROPERTIES TIMEOUT 90)
    add_test(NAME type_constraints_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/type_constraints_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(type_constraints_test PROPERTIES TIMEOUT 60)
    add_test(NAME backend_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(backend_test PROPERTIES TIMEOUT 60)
    add_test(NAME type_definitions_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/type_definitions_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(type_definitions_test PROPERTIES TIMEOUT 60)
    add_test(NAME maps_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/maps_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(maps_test PROPERTIES TIMEOUT 90)
    add_test(NAME mixed_arrays_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/mixed_arrays_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(mixed_arrays_test PROPERTIES TIMEOUT 60)
    add_test(NAME io_backend_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/io_backend_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(io_backend_test PROPERTIES TIMEOUT 60)
    add_test(NAME io_scanner_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/io_scanner_test.py" "$<TARGET_FILE:ocean-c>"
        "${OCEAN_CLANG_EXECUTABLE}" "$<TARGET_FILE:asm_backend_probe>")
    set_tests_properties(io_scanner_test PROPERTIES TIMEOUT 180)

    add_test(NAME range_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/range_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(range_test PROPERTIES TIMEOUT 60)
    add_test(NAME c_variadic_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/c_variadic_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(c_variadic_test PROPERTIES TIMEOUT 60)
    add_test(NAME package_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_test.py" "$<TARGET_FILE:ocean-c>")
    set_tests_properties(package_test PROPERTIES TIMEOUT 90)
    add_test(NAME os_args_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/os_args_test.py" "$<TARGET_FILE:ocean-c>")
    set_tests_properties(os_args_test PROPERTIES TIMEOUT 60)
    add_test(NAME inline_asm_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/inline_asm_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(inline_asm_test PROPERTIES TIMEOUT 60)
    add_test(NAME goto_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/goto_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(goto_test PROPERTIES TIMEOUT 60)
    add_test(NAME equality_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/equality_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(equality_test PROPERTIES TIMEOUT 90)
    find_package(Threads REQUIRED)
    add_executable(runtime_threads_test runtime_threads.c tests/runtime_threads_test.c)
    target_link_libraries(runtime_threads_test PRIVATE Threads::Threads)
    if(UNIX)
        # Darwin exposes wait() through its runtime headers. Make that namespace
        # visible on every POSIX test host to catch accidental libc collisions.
        target_compile_definitions(runtime_threads_test PRIVATE _POSIX_C_SOURCE=200809L)
        target_compile_options(runtime_threads_test PRIVATE -include sys/wait.h)
    endif()
    if(EXISTS "${OCEAN_STANDARD_LIBRARY_SOURCE}/os/exec/exec.oc")
        add_test(NAME release_smoke_test COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_smoke_test.py" "$<TARGET_FILE:ocean-c>")
        set_tests_properties(release_smoke_test PROPERTIES TIMEOUT 180
            ENVIRONMENT "OCEAN_SRC=${OCEAN_STANDARD_LIBRARY_SOURCE}")
    endif()
    add_test(NAME runtime_threads_test COMMAND runtime_threads_test)
    set_tests_properties(runtime_threads_test PROPERTIES TIMEOUT 30)
    if(EXISTS "${OCEAN_STANDARD_LIBRARY_SOURCE}/threads/tests/test_threads.py")
        add_test(NAME threads_test COMMAND ${CMAKE_COMMAND} -E env "OCEAN=$<TARGET_FILE:ocean-c>" "OCEAN_COMPILER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}"
            ${Python3_EXECUTABLE} "${OCEAN_STANDARD_LIBRARY_SOURCE}/threads/tests/test_threads.py")
        set_tests_properties(threads_test PROPERTIES TIMEOUT 180)
    endif()
    add_test(NAME nullable_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/nullable_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(nullable_test PROPERTIES TIMEOUT 90)
    add_test(NAME interface_dispatch_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/interface_dispatch_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(interface_dispatch_test PROPERTIES TIMEOUT 90)
    add_test(NAME closure_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/closure_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(closure_test PROPERTIES TIMEOUT 90)
    add_test(NAME defer_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/defer_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(defer_test PROPERTIES TIMEOUT 60)
    add_test(NAME panic_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/panic_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(panic_test PROPERTIES TIMEOUT 60)
    add_test(NAME array_bounds_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/array_bounds_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(array_bounds_test PROPERTIES TIMEOUT 60)
    add_test(NAME constant_builtins_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/constant_builtins_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(constant_builtins_test PROPERTIES TIMEOUT 60)
    add_test(NAME slicing_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/slicing_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(slicing_test PROPERTIES TIMEOUT 60)
    add_test(NAME array_addition_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/array_addition_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(array_addition_test PROPERTIES TIMEOUT 60)
    add_test(NAME string_dynamic_array_ownership_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/string_dynamic_array_ownership_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(string_dynamic_array_ownership_test PROPERTIES TIMEOUT 60)
    add_test(NAME new_allocation_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/new_allocation_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(new_allocation_test PROPERTIES TIMEOUT 60)
    add_test(NAME static_link_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/static_link_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(static_link_test PROPERTIES TIMEOUT 120)
    add_test(NAME build_cache_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_cache_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(build_cache_test PROPERTIES TIMEOUT 120)
    add_test(NAME reachability_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/reachability_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    add_test(NAME reflect_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/reflect_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(reflect_test PROPERTIES TIMEOUT 120)
    add_test(NAME runtime_array_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_array_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(runtime_array_test PROPERTIES TIMEOUT 60)
    add_test(NAME collection_types_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/collection_types_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(collection_types_test PROPERTIES TIMEOUT 60)
    add_test(NAME array_values_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/array_values_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(array_values_test PROPERTIES TIMEOUT 60)
    add_test(NAME enums_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/enums_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(enums_test PROPERTIES TIMEOUT 90)
endif()

if(BUILD_TESTING)
    find_program(OCEAN_WASM_LD_EXECUTABLE wasm-ld)
    if(OCEAN_WASM_LD_EXECUTABLE AND OCEAN_CLANG_EXECUTABLE)
        add_test(NAME wasm_backend_test COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_backend_test.py" "$<TARGET_FILE:ocean-c>")
        set_tests_properties(wasm_backend_test PROPERTIES TIMEOUT 120)
    endif()
endif()

# The CLI and C test/probe executables share native Windows compatibility.
if(WIN32)
    enable_language(RC)
    set_source_files_properties(windows/compat.c PROPERTIES COMPILE_DEFINITIONS OCEAN_COMPAT_IMPLEMENTATION)
    get_property(ocean_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(ocean_target IN LISTS ocean_targets)
        get_target_property(ocean_target_type ${ocean_target} TYPE)
        if(ocean_target_type STREQUAL "EXECUTABLE")
            target_sources(${ocean_target} PRIVATE windows/compat.c windows/ocean.rc)
            if(NOT ocean_target STREQUAL "ocean-c" AND NOT ocean_target STREQUAL "asm_backend_probe")
                target_sources(${ocean_target} PRIVATE platform.c)
            endif()
            target_include_directories(${ocean_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/windows/include")
            target_compile_options(${ocean_target} PRIVATE "$<$<COMPILE_LANGUAGE:C>:-include;${CMAKE_CURRENT_SOURCE_DIR}/windows/compat.h>")
            target_link_libraries(${ocean_target} PRIVATE bcrypt)
        endif()
    endforeach()
endif()

if(BUILD_TESTING AND OCEAN_CLANG_EXECUTABLE)
    add_test(NAME target_sources_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/target_sources_test.py" "$<TARGET_FILE:ocean-c>")
    set_tests_properties(target_sources_test PROPERTIES TIMEOUT 60)
endif()

if(BUILD_TESTING AND OCEAN_CLANG_EXECUTABLE)
    add_test(NAME library_containers_test COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/library_containers_test.py" "$<TARGET_FILE:ocean-c>" "${OCEAN_CLANG_EXECUTABLE}")
    set_tests_properties(library_containers_test PROPERTIES TIMEOUT 120)
    find_library(OCEAN_TEST_CURL_LIBRARY NAMES curl)
    if(OCEAN_TEST_CURL_LIBRARY AND EXISTS "${OCEAN_STANDARD_LIBRARY_SOURCE}/net/http/tests/linking.py")
        add_test(NAME http_linking_test COMMAND ${Python3_EXECUTABLE}
            "${OCEAN_STANDARD_LIBRARY_SOURCE}/net/http/tests/linking.py" "$<TARGET_FILE:ocean-c>")
        set_tests_properties(http_linking_test PROPERTIES TIMEOUT 120)
    endif()
endif()
