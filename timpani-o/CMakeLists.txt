# SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.5)

# Read version from VERSION file
# Try local VERSION first (for container builds), then parent directory
if(EXISTS "${CMAKE_SOURCE_DIR}/VERSION")
    file(READ "${CMAKE_SOURCE_DIR}/VERSION" VERSION_CONTENT)
elseif(EXISTS "${CMAKE_SOURCE_DIR}/../VERSION")
    file(READ "${CMAKE_SOURCE_DIR}/../VERSION" VERSION_CONTENT)
else()
    set(VERSION_CONTENT "0.0.0-dev")
    message(WARNING "VERSION file not found, using default: ${VERSION_CONTENT}")
endif()
string(STRIP "${VERSION_CONTENT}" PROJECT_VERSION_STRING)

# Project name and version
project(timpani-o VERSION ${PROJECT_VERSION_STRING} DESCRIPTION "Timpani-O Orchestrator")

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)

# Set cmake module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Add the binary dir to include dirs for gRPC/protobuf generated files
include_directories(${CMAKE_CURRENT_BINARY_DIR})

# Add local project components
add_subdirectory(proto)

# Add source directory
add_subdirectory(src)

# Option to build tests
option(BUILD_TESTS "Build unit tests" OFF)

# Add tests directory if building tests
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Packaging configuration
set(CPACK_PACKAGE_NAME "timpani-o")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "Timpani Developers")
include(CPack)
