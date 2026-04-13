## SPDX header
# SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.5)

# project name and version
project(timpani-n VERSION 2.0.0 DESCRIPTION "Timpani-n RT Execution Engine")

# Set cmake module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}")

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# BPF build option
option(CONFIG_TRACE_BPF "Enable eBPF sched_ext scheduler" ON)

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

install(TARGETS timpani-n DESTINATION bin)

include(CPack)
