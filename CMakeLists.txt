cmake_minimum_required(VERSION 3.16)
set(CMAKE_CONFIGURATION_TYPES Release CACHE STRING "" FORCE)
set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
project(NZXTKrakenLCDPlugin LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "NZXTKrakenLCDPlugin targets Windows x64 only.")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT DEFINED CMAKE_PREFIX_PATH AND NOT DEFINED Qt5_DIR AND NOT DEFINED ENV{CMAKE_PREFIX_PATH}
   AND EXISTS "C:/Qt/5.15.2/msvc2019_64")
    set(CMAKE_PREFIX_PATH "C:/Qt/5.15.2/msvc2019_64")
endif()
find_package(Qt5 5.15 REQUIRED COMPONENTS Core Gui Widgets)

foreach(_dep hidapi libusb)
    string(TOUPPER ${_dep} _DEP)
    if(NOT ${_DEP}_ROOT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${_dep}")
        set(${_DEP}_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${_dep}")
    endif()
endforeach()

find_path(HIDAPI_INCLUDE_DIR hidapi/hidapi.h HINTS ${HIDAPI_ROOT} PATH_SUFFIXES include)
find_library(HIDAPI_LIBRARY NAMES hidapi HINTS ${HIDAPI_ROOT} PATH_SUFFIXES lib)
find_path(LIBUSB_INCLUDE_DIR libusb-1.0/libusb.h HINTS ${LIBUSB_ROOT} PATH_SUFFIXES include)
find_library(LIBUSB_LIBRARY NAMES libusb-1.0 usb-1.0 HINTS ${LIBUSB_ROOT} PATH_SUFFIXES lib)
if(NOT HIDAPI_INCLUDE_DIR OR NOT HIDAPI_LIBRARY OR NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
    message(FATAL_ERROR "hidapi/libusb not found: pass -DHIDAPI_ROOT=... -DLIBUSB_ROOT=... or the vcpkg toolchain.")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lhwm-cpp-wrapper/lhwm-cpp-wrapper.h")
    set(_LHWM_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lhwm-cpp-wrapper")
else()
    set(_LHWM_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/../lhwm-wrapper/lhwm-cpp-wrapper/lhwm-cpp-wrapper")
endif()
set(LHWM_WRAPPER_DIR "${_LHWM_DEFAULT}"
    CACHE PATH "Folder containing lhwm-cpp-wrapper.h and x64/Release/lhwm-cpp-wrapper.lib")
if(NOT EXISTS "${LHWM_WRAPPER_DIR}/lhwm-cpp-wrapper.h"
   OR NOT EXISTS "${LHWM_WRAPPER_DIR}/x64/Release/lhwm-cpp-wrapper.lib")
    message(FATAL_ERROR "lhwm-cpp-wrapper not found in LHWM_WRAPPER_DIR=${LHWM_WRAPPER_DIR}")
endif()
get_filename_component(LHWM_WRAPPER_DIR "${LHWM_WRAPPER_DIR}" ABSOLUTE)

file(GLOB_RECURSE PLUGIN_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)
add_library(NZXTKrakenLCDPlugin SHARED ${PLUGIN_SOURCES})
target_include_directories(NZXTKrakenLCDPlugin PRIVATE src
    ${HIDAPI_INCLUDE_DIR} ${LIBUSB_INCLUDE_DIR} ${LHWM_WRAPPER_DIR})
target_compile_options(NZXTKrakenLCDPlugin PRIVATE /EHa /utf-8 /bigobj /W3)
target_compile_definitions(NZXTKrakenLCDPlugin PRIVATE NOMINMAX _USE_MATH_DEFINES
    _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING)
target_link_libraries(NZXTKrakenLCDPlugin PRIVATE
    Qt5::Core Qt5::Gui Qt5::Widgets
    ${HIDAPI_LIBRARY} ${LIBUSB_LIBRARY}
    "${LHWM_WRAPPER_DIR}/x64/Release/lhwm-cpp-wrapper.lib"
    hid setupapi winhttp ole32 windowsapp)
