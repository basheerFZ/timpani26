# SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
# SPDX-License-Identifier: MIT

# Timpani-O executable
add_executable(timpani-o
  main.cpp
  schedinfo_service.cpp
  fault_client.cpp
  node_config.cpp
  global_scheduler.cpp
  orchestrator_service.cpp
  recovery_service.cpp
)

target_link_libraries(timpani-o
  PRIVATE
    schedinfo_proto
    node_control_proto
)

install(TARGETS timpani-o
  RUNTIME DESTINATION bin
)

set_target_properties(timpani-o
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
