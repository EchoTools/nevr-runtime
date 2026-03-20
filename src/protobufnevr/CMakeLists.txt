# ProtobufNEVR/CMakeLists.txt
#
# Protobuf library using pre-generated code from extern/nevr-common
# 
# Uses protobuf from vcpkg

set(NEVRCOMMON_DIR "${CMAKE_CURRENT_LIST_DIR}/../../extern/nevr-common")
set(NEVRCOMMON_GEN_DIR "${NEVRCOMMON_DIR}/gen/cpp")

# Use pre-generated files from nevr-common (generated via protoc)
# Note: apigrpc.proto requires grpc-gateway dependencies, excluded for now
set(PROTO_GENERATED_SRCS
  ${NEVRCOMMON_GEN_DIR}/apigame/v1/engine_http_v1.pb.cc
  ${NEVRCOMMON_GEN_DIR}/api/v1/http_v1.pb.cc
  ${NEVRCOMMON_GEN_DIR}/rtapi/v1/realtime_v1.pb.cc
  ${NEVRCOMMON_GEN_DIR}/telemetry/v1/telemetry.pb.cc
  ${NEVRCOMMON_GEN_DIR}/types/v1/types_lobby_session.pb.cc
  ${NEVRCOMMON_GEN_DIR}/google/api/annotations.pb.cc
  ${NEVRCOMMON_GEN_DIR}/google/api/http.pb.cc
)
set(PROTO_GENERATED_HDRS
  ${NEVRCOMMON_GEN_DIR}/apigame/v1/engine_http_v1.pb.h
  ${NEVRCOMMON_GEN_DIR}/api/v1/http_v1.pb.h
  ${NEVRCOMMON_GEN_DIR}/rtapi/v1/realtime_v1.pb.h
  ${NEVRCOMMON_GEN_DIR}/telemetry/v1/telemetry.pb.h
  ${NEVRCOMMON_GEN_DIR}/types/v1/types_lobby_session.pb.h
  ${NEVRCOMMON_GEN_DIR}/google/api/annotations.pb.h
  ${NEVRCOMMON_GEN_DIR}/google/api/http.pb.h
)

# Set flag for parent CMakeLists to know if protobuf is available
set(PROTOBUF_AVAILABLE ON CACHE INTERNAL "Protobuf support available")

message(STATUS "Using pre-generated protobuf files from extern/nevr-common/gen/cpp")

# Create the library with pre-generated files (no codegen needed)
add_library(proto_nevr STATIC ${PROTO_GENERATED_SRCS} ${PROTO_GENERATED_HDRS})

# TODO: Switch to protobuf::libprotobuf-lite once well-known types (Timestamp)
# are available in lite form. Currently blocked by vcpkg's protobuf build.
target_link_libraries(proto_nevr PUBLIC protobuf::libprotobuf)

# Include directories - point to the gen/cpp directory
target_include_directories(proto_nevr PUBLIC
  $<BUILD_INTERFACE:${NEVRCOMMON_GEN_DIR}>
)

message(STATUS "Protobuf NEVR library configured successfully using pre-generated files")