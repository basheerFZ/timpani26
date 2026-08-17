# Load a module to generate protobuf/gRPC source files
include(GenProtobuf)

# Generate protobuf source files for gRPC
GEN_PROTOBUF("schedinfo.proto" PROTOBUF_GEN_FILES PROTOBUF_LIBS)

# Add the generated source files to the library project
add_library(schedinfo_proto ${PROTOBUF_GEN_FILES})
target_link_libraries(schedinfo_proto PUBLIC ${PROTOBUF_LIBS})

# Generate protobuf source files for node_control
GEN_PROTOBUF("node_control.proto" NODE_CONTROL_GEN_FILES NODE_CONTROL_LIBS)

# Add the generated source files to the library project
add_library(node_control_proto ${NODE_CONTROL_GEN_FILES})
target_link_libraries(node_control_proto PUBLIC ${NODE_CONTROL_LIBS})
