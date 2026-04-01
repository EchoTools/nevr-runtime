# Protobuf library using pre-generated code from gen/cpp
#
# Generated from buf.build/echotools/nevr-api via:
#   buf generate buf.build/echotools/nevr-api

set(GEN_DIR "${CMAKE_SOURCE_DIR}/gen/cpp")

set(PROTO_GENERATED_SRCS
  ${GEN_DIR}/engine/v1/engine_http.pb.cc
  ${GEN_DIR}/gameservice/v1/friends.pb.cc
  ${GEN_DIR}/gameservice/v1/gameservice.pb.cc
  ${GEN_DIR}/gameservice/v1/party.pb.cc
  ${GEN_DIR}/spatial/v1/types.pb.cc
  ${GEN_DIR}/telemetry/v1/telemetry.pb.cc
  ${GEN_DIR}/telemetry/v2/capture.pb.cc
  ${GEN_DIR}/telemetry/v2/echo_arena.pb.cc
)

add_library(nevr_api STATIC ${PROTO_GENERATED_SRCS})

target_link_libraries(nevr_api PUBLIC protobuf::libprotobuf)

target_include_directories(nevr_api PUBLIC
  $<BUILD_INTERFACE:${GEN_DIR}>
)
