## SPDX header
# SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.5)

# Read version from VERSION file
# Try local VERSION first (for container/package builds), then parent directory
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
project(timpani-n VERSION ${PROJECT_VERSION_STRING} DESCRIPTION "Timpani-n RT Execution Engine")

# Get Git commit hash for version tracking
# First check if passed via -DGIT_COMMIT_HASH (for container/package builds)
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

# Set cmake module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}")

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# BPF build option
option(CONFIG_TRACE_BPF "Enable eBPF sched_ext scheduler" ON)
option(CONFIG_TRACE_BPF_EVENT "Enable eBPF schedstat tracing for gpdata output" OFF)

set(TIMPANI_ENABLE_GPDATA OFF)
if(CONFIG_TRACE_BPF_EVENT AND NOT CONFIG_TRACE_BPF)
    message(WARNING "CONFIG_TRACE_BPF_EVENT=ON requires CONFIG_TRACE_BPF=ON. gpdata tracing will be disabled.")
endif()
if(CONFIG_TRACE_BPF AND CONFIG_TRACE_BPF_EVENT)
    set(TIMPANI_ENABLE_GPDATA ON)
endif()

# GRPC / Protobuf definitions for node_control.proto
set(TIMPANI_O_PROTO_DIR "${CMAKE_SOURCE_DIR}/../timpani-o/proto")
find_package(gRPC QUIET)
if (gRPC_FOUND)
    set(NODE_GRPC_LIB gRPC::grpc++)
else()
    find_package(PkgConfig REQUIRED)
    pkg_search_module(GRPC REQUIRED IMPORTED_TARGET grpc)
    pkg_search_module(GRPCPP REQUIRED IMPORTED_TARGET grpc++)
    set(NODE_GRPC_LIB PkgConfig::GRPCPP)
endif()
find_package(Protobuf REQUIRED)

find_program(GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin)
if(NOT GRPC_CPP_PLUGIN_EXECUTABLE)
    message(FATAL_ERROR "grpc_cpp_plugin not found")
endif()

set(PROTO_FILE "${TIMPANI_O_PROTO_DIR}/node_control.proto")
set(NODE_CONTROL_PROTO_SRC "${CMAKE_BINARY_DIR}/node_control.pb.cc")
set(NODE_CONTROL_PROTO_HDR "${CMAKE_BINARY_DIR}/node_control.pb.h")
set(NODE_CONTROL_GRPC_SRC "${CMAKE_BINARY_DIR}/node_control.grpc.pb.cc")
set(NODE_CONTROL_GRPC_HDR "${CMAKE_BINARY_DIR}/node_control.grpc.pb.h")

add_custom_command(
    OUTPUT "${NODE_CONTROL_PROTO_SRC}" "${NODE_CONTROL_PROTO_HDR}" "${NODE_CONTROL_GRPC_SRC}" "${NODE_CONTROL_GRPC_HDR}"
    COMMAND ${PROTOBUF_PROTOC_EXECUTABLE}
    ARGS --grpc_out="${CMAKE_BINARY_DIR}"
         --cpp_out="${CMAKE_BINARY_DIR}"
         -I "${TIMPANI_O_PROTO_DIR}"
         --plugin=protoc-gen-grpc="${GRPC_CPP_PLUGIN_EXECUTABLE}"
         "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}"
)

if(CONFIG_TRACE_BPF)
    message(STATUS "CONFIG_TRACE_BPF is ON - Building sched_ext BPF scheduler")

    include(ExternalProject)

    # Path to the shared libbpf in the TIMPANI root
    set(SHARED_LIBBPF_PATH "${CMAKE_SOURCE_DIR}/../libbpf")

    # Manually define libbpf project to point to the shared root
    ExternalProject_Add(libbpf
        SOURCE_DIR "${SHARED_LIBBPF_PATH}"
        CONFIGURE_COMMAND ""
        BUILD_COMMAND make -C <SOURCE_DIR>/src BUILD_STATIC_ONLY=1 DESTDIR=<BINARY_DIR> OBJDIR=<BINARY_DIR>/obj install
        INSTALL_COMMAND ""
        BINARY_DIR "${CMAKE_BINARY_DIR}/libbpf"
    )

    # Load bpf.cmake
    include(bpf)

    # BPF include path for clang
    set(BPF_EXTRA_CFLAGS "-I${SHARED_LIBBPF_PATH}/src")

    add_compile_definitions(CONFIG_TRACE_BPF=1)

    include_directories(${CMAKE_BINARY_DIR}/libbpf/usr/include)

    # Build sched_ext BPF scheduler (timpani.bpf.c)
    ADD_BPF(src/bpf_loader.cpp src/bpf/timpani.bpf.c timpani.bpf.o timpani.skel.h)

    if(TIMPANI_ENABLE_GPDATA)
        message(STATUS "CONFIG_TRACE_BPF_EVENT is ON - Building schedstat BPF tracer")
        add_compile_definitions(CONFIG_TRACE_BPF_EVENT=1)
        ADD_BPF(src/schedstat_monitor.cpp src/bpf/schedstat.bpf.c schedstat.bpf.o schedstat.skel.h)
    endif()
endif()

# timpani-n C++ sources (DDR-006 rework)
set(TIMPANI_N_SOURCES
    src/main.cpp
    src/timer_master.cpp
    src/bpf_loader.cpp
    src/fault_monitor.cpp
    src/task_registry.cpp
    src/grpc/node_client.cpp
    ${NODE_CONTROL_PROTO_SRC}
    ${NODE_CONTROL_GRPC_SRC}
)

if(TIMPANI_ENABLE_GPDATA)
    list(APPEND TIMPANI_N_SOURCES
        src/gpdata_writer.cpp
        src/schedstat_monitor.cpp
    )
endif()

add_executable(timpani-n ${TIMPANI_N_SOURCES})

# BPF dependencies
if(CONFIG_TRACE_BPF)
    add_dependencies(timpani-n libbpf)
    target_include_directories(timpani-n PRIVATE
        ${CMAKE_BINARY_DIR}
        ${CMAKE_BINARY_DIR}/libbpf/usr/include
    )
    target_link_libraries(timpani-n
        ${CMAKE_BINARY_DIR}/libbpf/obj/libbpf.a
        elf z
    )
endif()

# Optimization
target_compile_options(timpani-n PRIVATE -O3 -flto -funroll-loops -ffast-math -fomit-frame-pointer)
set_target_properties(timpani-n PROPERTIES LINK_FLAGS "-flto -O3")

# Link libraries
target_link_libraries(timpani-n
    ${NODE_GRPC_LIB}
    protobuf::libprotobuf
    pthread
    rt
)

target_include_directories(timpani-n
    PUBLIC ${CMAKE_SOURCE_DIR}/src
    PUBLIC ${CMAKE_BINARY_DIR}
)

    # ============================================================================
    # Installation
    # ============================================================================

    # Binary
    install(TARGETS timpani-n DESTINATION bin)

    # Systemd service file
    install(FILES ${CMAKE_SOURCE_DIR}/timpani-n.service
        DESTINATION lib/systemd/system)

    # Configuration file
    install(FILES ${CMAKE_SOURCE_DIR}/timpani-n.conf
        DESTINATION /etc/timpani)

    # ============================================================================
    # CPack Configuration
    # ============================================================================

    set(CPACK_PACKAGE_NAME "timpani-n")
    set(CPACK_PACKAGE_VENDOR "LG Electronics")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "TIMPANI Node Executor - Real-time Scheduler with BPF/sched_ext")
    set(CPACK_PACKAGE_DESCRIPTION "TIMPANI-N is a real-time task executor that uses eBPF and sched_ext for deterministic scheduling. It connects to TIMPANI-O orchestrator via gRPC to receive schedule tables and executes time-triggered tasks with microsecond precision.")
    set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/mco-piccolo/timpani")
    set(CPACK_PACKAGE_CONTACT "seokhee.han@lge.com")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/../LICENSE")

    # Package file name format
    set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}_${PROJECT_VERSION}_${CMAKE_SYSTEM_PROCESSOR}")

    # ============================================================================
    # DEB Package Configuration
    # ============================================================================

    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Seokhee Han <seokhee.han@lge.com>")
    set(CPACK_DEBIAN_PACKAGE_SECTION "admin")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")

    # Dependencies (Ubuntu 22.04+)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libelf1, zlib1g, libprotobuf23 | libprotobuf32, libgrpc++1 | libgrpc++1.51")

    # Post-install/remove scripts
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CMAKE_SOURCE_DIR}/packaging/postinst;${CMAKE_SOURCE_DIR}/packaging/prerm;${CMAKE_SOURCE_DIR}/packaging/postrm")

    # ============================================================================
    # RPM Package Configuration
    # ============================================================================

    set(CPACK_RPM_PACKAGE_LICENSE "MIT")
    set(CPACK_RPM_PACKAGE_GROUP "System Environment/Daemons")
    set(CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")

    # Dependencies (CentOS Stream 10)
    set(CPACK_RPM_PACKAGE_REQUIRES "elfutils-libelf, zlib, protobuf, grpc")

    # Post-install/remove scripts
    set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${CMAKE_SOURCE_DIR}/packaging/postinst")
    set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE "${CMAKE_SOURCE_DIR}/packaging/prerm")
    set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE "${CMAKE_SOURCE_DIR}/packaging/postrm")

include(CPack)
