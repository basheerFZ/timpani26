## SPDX header
# SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.5)

# project name and version
project(timpani-n VERSION 2.0.0 DESCRIPTION "Timpani-n")

# Set cmake module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}")

# BPF tracing build options
option(CONFIG_TRACE_BPF "Enable eBPF support" ON)
option(CONFIG_TRACE_BPF_EVENT "Enable eBPF-based event tracing" OFF)

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
    message("CONFIG_TRACE_BPF is ON")

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

    # Load bpf.cmake - we must help clang find headers in the shared root
    include(bpf)

    # We pass the shared path to Clang so #include <bpf/bpf_helpers.h> works
    # We use -I.../src because the 'bpf' folder is inside 'src'
    set(BPF_EXTRA_CFLAGS "-I${SHARED_LIBBPF_PATH}/src")

    add_compile_definitions(CONFIG_TRACE_BPF=1)

    # IMPORTANT: We need to make sure the ADD_BPF macro uses our new path
    # If sigwait.bpf.c fails to find headers, we might need to edit bpf.cmake
    include_directories(${CMAKE_BINARY_DIR}/libbpf/usr/include)
    ADD_BPF(src/trace_bpf.c src/sigwait.bpf.c sigwait.bpf.o sigwait.skel.h)

    if(CONFIG_TRACE_BPF_EVENT)
        message("CONFIG_TRACE_BPF_EVENT is ON")
        add_compile_definitions(CONFIG_TRACE_BPF_EVENT=1)
        # Add this right before ADD_BPF in CMakeLists.txt
        ADD_BPF(src/trace_bpf.c src/schedstat.bpf.c schedstat.bpf.o schedstat.skel.h)
    endif()
endif()

# timpani-n sources
set(TIMPANI_N_SOURCES
    src/main.c
    src/core.c
    src/hyperperiod.c
    src/task.c
    src/config.c
    src/trpc.c
    src/signal.c
    src/cleanup.c
    src/sched.c
    src/globals.c
    src/apex_monitor.c
)

if(CONFIG_TRACE_BPF)
    set(TIMPANI_N_SOURCES ${TIMPANI_N_SOURCES} src/trace_bpf.c)
endif()

add_executable(timpani-n ${TIMPANI_N_SOURCES})

# timpani-n-cpp (DDR-006 C++ rework skeleton)
set(TIMPANI_N_CPP_SOURCES
    src/main.cpp
    src/timer_master.cpp
    src/bpf_loader.cpp
    src/fault_monitor.cpp
    src/task_registry.cpp
    src/grpc/node_client.cpp
    ${NODE_CONTROL_PROTO_SRC}
    ${NODE_CONTROL_GRPC_SRC}
)
add_executable(timpani-n-cpp ${TIMPANI_N_CPP_SOURCES})

# BPF dependencies
if(CONFIG_TRACE_BPF)
    add_dependencies(timpani-n libbpf)
    add_dependencies(timpani-n-cpp libbpf)
    target_include_directories(timpani-n PRIVATE
        ${CMAKE_BINARY_DIR}
        ${CMAKE_BINARY_DIR}/libbpf/usr/include
    )
    target_include_directories(timpani-n-cpp PRIVATE
        ${CMAKE_BINARY_DIR}
        ${CMAKE_BINARY_DIR}/libbpf/usr/include
    )
    target_link_libraries(timpani-n
        ${CMAKE_BINARY_DIR}/libbpf/obj/libbpf.a
        pthread elf z
    )
    target_link_libraries(timpani-n-cpp
        ${CMAKE_BINARY_DIR}/libbpf/obj/libbpf.a
        pthread elf z
    )
endif()

# Optimization
target_compile_options(timpani-n PRIVATE -O3 -flto -funroll-loops -ffast-math -fomit-frame-pointer)
target_compile_options(timpani-n-cpp PRIVATE -O3 -flto -funroll-loops -ffast-math -fomit-frame-pointer)
set_target_properties(timpani-n PROPERTIES LINK_FLAGS "-flto -O3")
set_target_properties(timpani-n-cpp PROPERTIES LINK_FLAGS "-flto -O3")

# Handle libtrpc (Sibling folder)
if(EXISTS "${CMAKE_SOURCE_DIR}/../libtrpc")
    add_subdirectory("${CMAKE_SOURCE_DIR}/../libtrpc" "${CMAKE_BINARY_DIR}/libtrpc" EXCLUDE_FROM_ALL)
endif()

target_link_libraries(timpani-n trpc systemd rt)
target_include_directories(timpani-n
    PUBLIC ${CMAKE_SOURCE_DIR}/src
    PUBLIC ${CMAKE_SOURCE_DIR}/../libtrpc/src
)

target_link_libraries(timpani-n-cpp
    ${NODE_GRPC_LIB}
    protobuf::libprotobuf
    systemd rt
)
target_include_directories(timpani-n-cpp
    PUBLIC ${CMAKE_SOURCE_DIR}/src
    PUBLIC ${CMAKE_BINARY_DIR}
)

install(TARGETS timpani-n timpani-n-cpp DESTINATION bin)
add_executable(exprocs EXCLUDE_FROM_ALL test/exprocs.c)

include(CPack)
