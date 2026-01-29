# ProtobufNEVR/CMakeLists.txt
#
# Protobuf code generation and library
# 
# Uses protobuf from vcpkg

set(NEVRCOMMON_DIR "${CMAKE_CURRENT_LIST_DIR}/../../extern/nevr-common")

set(PROTO_IMPORT_DIRS
  ${NEVRCOMMON_DIR}/proto
)

# Include all relevant proto files
set(PROTO_FILES
  ${NEVRCOMMON_DIR}/proto/google/api/http.proto
  ${NEVRCOMMON_DIR}/proto/google/api/annotations.proto
  ${NEVRCOMMON_DIR}/proto/apigame/http_v1.proto
  ${NEVRCOMMON_DIR}/proto/rtapi/realtime_v1.proto
  ${NEVRCOMMON_DIR}/proto/telemetry/v1/telemetry.proto
)
set(PROTO_BINARY_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${PROTO_BINARY_DIR})

# Generated files - names match the proto file paths
set(PROTO_GENERATED_SRCS
  ${PROTO_BINARY_DIR}/google/api/http.pb.cc
  ${PROTO_BINARY_DIR}/google/api/annotations.pb.cc
  ${PROTO_BINARY_DIR}/apigame/http_v1.pb.cc
  ${PROTO_BINARY_DIR}/rtapi/realtime_v1.pb.cc
  ${PROTO_BINARY_DIR}/telemetry/v1/telemetry.pb.cc
)
set(PROTO_GENERATED_HDRS
  ${PROTO_BINARY_DIR}/google/api/http.pb.h
  ${PROTO_BINARY_DIR}/google/api/annotations.pb.h
  ${PROTO_BINARY_DIR}/apigame/http_v1.pb.h
  ${PROTO_BINARY_DIR}/rtapi/realtime_v1.pb.h
  ${PROTO_BINARY_DIR}/telemetry/v1/telemetry.pb.h
)

# Set flag for parent CMakeLists to know if protobuf is available
set(PROTOBUF_AVAILABLE ON CACHE INTERNAL "Protobuf support available")

# Protobuf from vcpkg - find_package was called in extern/CMakeLists.txt
# Protobuf_PROTOC_EXECUTABLE is set by the find_package call

message(STATUS "Using protoc: ${Protobuf_PROTOC_EXECUTABLE}")

# Generate protobuf files using protoc
add_custom_command(
  OUTPUT ${PROTO_GENERATED_SRCS} ${PROTO_GENERATED_HDRS}
  COMMAND ${Protobuf_PROTOC_EXECUTABLE}
          --proto_path=${NEVRCOMMON_DIR}/proto
          --cpp_out=${PROTO_BINARY_DIR}
          ${PROTO_FILES}
  DEPENDS ${PROTO_FILES}
  COMMENT "Generating protobuf files with vcpkg protoc"
  VERBATIM
)

# Create the library with generated files
add_library(proto_nevr STATIC ${PROTO_GENERATED_SRCS} ${PROTO_GENERATED_HDRS})

# Link to protobuf library from vcpkg
target_link_libraries(proto_nevr PUBLIC protobuf::libprotobuf)

# Include directories
target_include_directories(proto_nevr PUBLIC
  $<BUILD_INTERFACE:${PROTO_BINARY_DIR}>
)

message(STATUS "Protobuf NEVR library configured successfully with vcpkg")