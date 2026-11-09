# Banc de mesure du rendu/encodage LCD (sans matériel).
#   cmake -S tests -B build-bench -G "NMake Makefiles" -DLHWM_WRAPPER_DIR=<dossier lhwm-cpp-wrapper>
#   cmake --build build-bench && build-bench\bench_render.exe 200
cmake_minimum_required(VERSION 3.16)
set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
project(NZXTKrakenLCDBench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(NOT DEFINED CMAKE_PREFIX_PATH AND EXISTS "C:/Qt/5.15.2/msvc2019_64")
    set(CMAKE_PREFIX_PATH "C:/Qt/5.15.2/msvc2019_64")
endif()
find_package(Qt5 5.15 REQUIRED COMPONENTS Core Gui)

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(NOT LHWM_WRAPPER_DIR)
    set(LHWM_WRAPPER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lhwm-cpp-wrapper")
endif()
add_executable(bench_render bench_render.cpp
    ${SRC}/renderer/FrameRenderer.cpp ${SRC}/sensors/SystemSensors.cpp)
target_include_directories(bench_render PRIVATE ${SRC} ${LHWM_WRAPPER_DIR})
target_compile_options(bench_render PRIVATE /EHa /utf-8 /bigobj)
target_compile_definitions(bench_render PRIVATE NOMINMAX _USE_MATH_DEFINES)
target_link_libraries(bench_render PRIVATE Qt5::Core Qt5::Gui
    "${LHWM_WRAPPER_DIR}/x64/Release/lhwm-cpp-wrapper.lib")
