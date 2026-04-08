cmake_minimum_required(VERSION 3.20)
project(memory-visualizer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra)

add_library(sim_core OBJECT
    core.cpp
    uncore.cpp
    cpu.cpp
    memory.cpp
    os.cpp
    cpu_callback.cpp
    event.cpp
)
target_include_directories(sim_core PUBLIC ${CMAKE_SOURCE_DIR})

find_package(SDL3 REQUIRED)
add_executable(visualizer
    visualizer_main.cpp
    renderer.cpp
    $<TARGET_OBJECTS:sim_core>
)
target_link_libraries(visualizer PRIVATE SDL3::SDL3)

file(GLOB BENCH_CXX_SRCS CONFIGURE_DEPENDS benchmarks/*.cpp)
foreach(src ${BENCH_CXX_SRCS})
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} ${src} $<TARGET_OBJECTS:sim_core>)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR})
endforeach()

foreach(src ${BENCH_CXX_SRCS})
    get_filename_component(name ${src} NAME_WE)
    add_custom_target(run_${name}
        COMMAND $<TARGET_FILE:${name}> ${CMAKE_BINARY_DIR}/${name}.trace
        COMMAND $<TARGET_FILE:visualizer> ${CMAKE_BINARY_DIR}/${name}.trace
        DEPENDS ${name} visualizer
        COMMENT "Recording and visualizing ${name}"
    )
endforeach()
