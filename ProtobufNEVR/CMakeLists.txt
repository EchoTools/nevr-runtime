find_package(Protobuf CONFIG REQUIRED)

set(COMMON_DIR "${CMAKE_CURRENT_LIST_DIR}/../nevr-common.git")
add_library(proto-objects OBJECT "${COMMON_DIR}/gameapi/gameapi.proto"
                                 "${COMMON_DIR}/rtapi/rtapi.proto")

target_link_libraries(proto-objects PUBLIC protobuf::libprotobuf)

set(PROTO_BINARY_DIR "${CMAKE_BINARY_DIR}/generated")
set(PROTO_IMPORT_DIRS "${COMMON_DIR}/gameapi" "${COMMON_DIR}/rtapi")

protobuf_generate(
  TARGET
  proto-objects
  OUT_VAR
  PROTO_SRCS
  IMPORT_DIRS
  ${PROTO_IMPORT_DIRS}
  PROTOC_OUT_DIR
  "${PROTO_BINARY_DIR}")

set_source_files_properties(${PROTO_SRCS} PROPERTIES SKIP_UNITY_BUILD_INCLUSION
                                                     on)
add_library(proto_nevr STATIC ${PROTO_SRCS})
target_include_directories(proto_nevr
                           PUBLIC $<BUILD_INTERFACE:${PROTO_BINARY_DIR}>)

target_link_libraries(proto_nevr PUBLIC protobuf::libprotobuf)
target_include_directories(proto_nevr PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
