cmake_minimum_required(VERSION 3.6)
## SPDX header
# SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.6)

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

# project name and version
project(sample_apps VERSION ${PROJECT_VERSION_STRING} DESCRIPTION "Timpani Sample Apps")

# Get Git commit hash for version tracking
# First check if passed via -DGIT_COMMIT_HASH (for container builds)
if(NOT DEFINED GIT_COMMIT_HASH OR GIT_COMMIT_HASH STREQUAL "unknown")
    execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
if(NOT GIT_COMMIT_HASH)
    set(GIT_COMMIT_HASH "unknown")
endif()

# Get build timestamp
string(TIMESTAMP BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S UTC" UTC)

# Generate version header with actual source info
configure_file(
    ${CMAKE_SOURCE_DIR}/src/version.h.in
    ${CMAKE_BINARY_DIR}/version.h
    @ONLY
)
include_directories(${CMAKE_BINARY_DIR})

# library install config
install(TARGETS
	ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# sample_apps program
add_library(ttsched SHARED src/libttsched.c)
target_include_directories(ttsched PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(sample_apps src/sample_apps.c)
target_include_directories(sample_apps
			PUBLIC ${CMAKE_SOURCE_DIR}/src
            PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(sample_apps
		PUBLIC ${LIBSYSTEMD_CFLAGS_OTHER}
)
target_link_libraries(sample_apps PUBLIC m ttsched)
