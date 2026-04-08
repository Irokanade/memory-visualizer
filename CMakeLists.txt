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
set_target_properties(sim_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(cachesim SHARED
    cachesim.cpp
    $<TARGET_OBJECTS:sim_core>
)
target_include_directories(cachesim PUBLIC ${CMAKE_SOURCE_DIR})

find_package(LLVM REQUIRED CONFIG
    HINTS $ENV{LLVM_DIR} /opt/homebrew/opt/llvm/lib/cmake/llvm
)
add_library(mem_trace_pass MODULE mem_trace_pass.cpp)
target_include_directories(mem_trace_pass PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(mem_trace_pass PRIVATE ${LLVM_DEFINITIONS})
target_compile_options(mem_trace_pass PRIVATE -fno-exceptions)
if(APPLE)
    target_link_options(mem_trace_pass PRIVATE -undefined dynamic_lookup)
endif()

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

find_program(LLVM_CC clang HINTS ${LLVM_TOOLS_BINARY_DIR} REQUIRED)

file(GLOB BENCH_C_SRCS CONFIGURE_DEPENDS benchmarks/*.c)
foreach(src ${BENCH_C_SRCS})
    get_filename_component(name ${src} NAME_WE)
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/${name}
        COMMAND ${LLVM_CC} -O0
            -fpass-plugin=$<TARGET_FILE:mem_trace_pass>
            -L${CMAKE_BINARY_DIR} -lcachesim
            -Wl,-rpath,${CMAKE_BINARY_DIR}
            ${src} -o ${CMAKE_BINARY_DIR}/${name}
        DEPENDS ${src} cachesim mem_trace_pass
        COMMENT "Instrumenting ${name}"
        VERBATIM
    )
    list(APPEND BENCH_TARGETS ${CMAKE_BINARY_DIR}/${name})
endforeach()
add_custom_target(benchmarks DEPENDS ${BENCH_TARGETS})

foreach(src ${BENCH_C_SRCS})
    get_filename_component(name ${src} NAME_WE)
    add_custom_target(run_${name}
        COMMAND ${CMAKE_COMMAND} -E env
            CACHESIM_TRACE=${CMAKE_BINARY_DIR}/${name}.trace
            ${CMAKE_BINARY_DIR}/${name}
        COMMAND $<TARGET_FILE:visualizer> ${CMAKE_BINARY_DIR}/${name}.trace
        DEPENDS ${CMAKE_BINARY_DIR}/${name} visualizer
        COMMENT "Recording and visualizing ${name}"
    )
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
