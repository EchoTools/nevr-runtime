# In ProtobufNEVR/CMakeLists.txt

find_package(Protobuf REQUIRED)


# 1. Create the empty static library target first.
#    The generated files will be added to it.
add_library(proto_nevr STATIC)

set(NEVRCOMMON_DIR "${CMAKE_CURRENT_LIST_DIR}/../nevr-common.git")
# Explicitly list only the required .proto files with relative paths.
file(GLOB PROTO_FILES
    "${NEVRCOMMON_DIR}/*/*.proto"
)
# only include nevr_rtapi.proto and nevr_gameapi.proto
set(PROTO_FILES
  ${NEVRCOMMON_DIR}/gameapi/nevr_gameapi.proto
  ${NEVRCOMMON_DIR}/rtapi/nevr_rtapi.proto
)
set(PROTO_BINARY_DIR "${CMAKE_BINARY_DIR}/generated")
# Set the import directories so the protobuf compiler can find dependencies.
set(PROTO_IMPORT_DIRS
  ${NEVRCOMMON_DIR}
)

protobuf_generate(
  TARGET proto_nevr
  PROTOC_OUT_DIR ${PROTO_BINARY_DIR}
  PROTOS ${PROTO_FILES}
  IMPORT_DIRS  ${NEVRCOMMON_DIR}
)

target_link_libraries(proto_nevr PUBLIC protobuf::libprotobuf)

target_include_directories(proto_nevr PUBLIC
    $<BUILD_INTERFACE:${PROTO_BINARY_DIR}>
)