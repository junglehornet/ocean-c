cmake_minimum_required(VERSION 4.2)
project(ocean-c C)

set(CMAKE_C_STANDARD 23)

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