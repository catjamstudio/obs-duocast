cmake_minimum_required(VERSION 3.24)
project(OBSDuoCast VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_path(LIBOBS_INCLUDE_DIR obs-module.h
    HINTS "$ENV{OBS_SDK_ROOT}/include" "$ENV{OBS_ROOT}/include")
find_library(LIBOBS_LIBRARY NAMES libobs obs
    HINTS "$ENV{OBS_SDK_ROOT}/lib" "$ENV{OBS_ROOT}/lib")
find_path(TURBOJPEG_INCLUDE_DIR turbojpeg.h
    HINTS "$ENV{TURBOJPEG_ROOT}/include" "$ENV{TURBOJPEG_ROOT}/sdk/include")
find_library(TURBOJPEG_LIBRARY NAMES turbojpeg
    HINTS "$ENV{TURBOJPEG_ROOT}/lib" "$ENV{TURBOJPEG_ROOT}/sdk/lib")

if(NOT LIBOBS_INCLUDE_DIR OR NOT LIBOBS_LIBRARY OR NOT TURBOJPEG_INCLUDE_DIR OR NOT TURBOJPEG_LIBRARY)
    message(FATAL_ERROR
        "OBS DuoCast requires the OBS Studio development SDK and libjpeg-turbo 3.2.0. "
        "Set OBS_SDK_ROOT and TURBOJPEG_ROOT to their SDK locations.")
endif()

add_library(OBSDuoCast MODULE
    src/plugin.cpp
    src/protocol.cpp
    src/sender_output.cpp
    src/receiver_source.cpp)

target_include_directories(OBSDuoCast PRIVATE
    "${LIBOBS_INCLUDE_DIR}"
    "${TURBOJPEG_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(OBSDuoCast PRIVATE "${LIBOBS_LIBRARY}" "${TURBOJPEG_LIBRARY}")
target_compile_definitions(OBSDuoCast PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
if(WIN32)
    target_link_libraries(OBSDuoCast PRIVATE ws2_32)
endif()

set_target_properties(OBSDuoCast PROPERTIES
    PREFIX ""
    OUTPUT_NAME "OBS-DuoCast")

if(WIN32)
    set(DUOCAST_PLUGIN_DIR "${CMAKE_BINARY_DIR}/OBS-DuoCast/obs-plugins/64bit")
    set(DUOCAST_DATA_DIR "${CMAKE_BINARY_DIR}/OBS-DuoCast/data/obs-plugins/OBS-DuoCast/locale")
    add_custom_command(TARGET OBSDuoCast POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DUOCAST_PLUGIN_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DUOCAST_DATA_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:OBSDuoCast>" "${DUOCAST_PLUGIN_DIR}/$<TARGET_FILE_NAME:OBSDuoCast>"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/data/locale/en-US.ini" "${DUOCAST_DATA_DIR}/en-US.ini")
endif()
