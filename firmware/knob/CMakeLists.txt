cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/version.cmake)
if(NOT "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}" VERSION_EQUAL "5.5.6")
    message(FATAL_ERROR "ESP-IDF 5.5.6 is required; select it with IDF_PATH")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lx_knob_idf)
