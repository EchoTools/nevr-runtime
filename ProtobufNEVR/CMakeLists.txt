# In ProtobufNEVR/CMakeLists.txt

find_package(Protobuf CONFIG REQUIRED)

set(NEVRCOMMON_DIR "${CMAKE_CURRENT_LIST_DIR}/../nevr-common.git")

# only include nevr_rtapi.proto and nevr_gameapi.proto
set(PROTO_FILES
  ${NEVRCOMMON_DIR}/gameapi/nevr_gameapi.proto
  ${NEVRCOMMON_DIR}/rtapi/nevr_rtapi.proto
)
set(PROTO_BINARY_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${PROTO_BINARY_DIR})

# For cross-compilation from Linux, use vcpkg protoc.exe via Wine wrapper
# to ensure version compatibility with vcpkg protobuf library
if(CMAKE_CROSSCOMPILING)
  set(VCPKG_PROTOC "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/tools/protobuf/protoc.exe")
  set(PROTOC_WRAPPER "${CMAKE_SOURCE_DIR}/scripts/protoc-wine.sh")
  
  if(EXISTS ${VCPKG_PROTOC} AND EXISTS ${PROTOC_WRAPPER})
    message(STATUS "Using vcpkg protoc via Wine wrapper for code generation")
    
    # Generated files
    set(PROTO_GENERATED_SRCS
      ${PROTO_BINARY_DIR}/gameapi/nevr_gameapi.pb.cc
      ${PROTO_BINARY_DIR}/rtapi/nevr_rtapi.pb.cc
    )
    set(PROTO_GENERATED_HDRS
      ${PROTO_BINARY_DIR}/gameapi/nevr_gameapi.pb.h
      ${PROTO_BINARY_DIR}/rtapi/nevr_rtapi.pb.h
    )
    
    # Custom command to generate protobuf files
    add_custom_command(
      OUTPUT ${PROTO_GENERATED_SRCS} ${PROTO_GENERATED_HDRS}
      COMMAND ${PROTOC_WRAPPER} ${VCPKG_PROTOC}
              --proto_path=${NEVRCOMMON_DIR}
              --cpp_out=${PROTO_BINARY_DIR}
              ${PROTO_FILES}
      DEPENDS ${PROTO_FILES}
      COMMENT "Generating protobuf files via Wine"
      VERBATIM
    )
    
    # Create the library with generated files
    add_library(proto_nevr STATIC ${PROTO_GENERATED_SRCS} ${PROTO_GENERATED_HDRS})
    
  else()
    message(FATAL_ERROR "vcpkg protoc or wrapper not found for cross-compilation")
  endif()
else()
  # Native build - use protobuf_generate
  add_library(proto_nevr STATIC)
  
  protobuf_generate(
    TARGET proto_nevr
    PROTOC_OUT_DIR ${PROTO_BINARY_DIR}
    PROTOS ${PROTO_FILES}
    IMPORT_DIRS ${NEVRCOMMON_DIR}
  )
endif()

target_link_libraries(proto_nevr PUBLIC protobuf::libprotobuf)

target_include_directories(proto_nevr PUBLIC
  $<BUILD_INTERFACE:${PROTO_BINARY_DIR}>
)