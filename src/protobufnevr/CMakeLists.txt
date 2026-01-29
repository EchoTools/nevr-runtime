# In ProtobufNEVR/CMakeLists.txt

find_package(Protobuf CONFIG REQUIRED)

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

# For cross-compilation from Linux, use vcpkg protoc (native Linux binary)
# to ensure version compatibility with vcpkg protobuf library
if(CMAKE_CROSSCOMPILING)
  # Use the native protoc from vcpkg (x64-linux or x64-mingw-static)
  set(VCPKG_PROTOC "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-mingw-static/tools/protobuf/protoc")
  
  if(EXISTS ${VCPKG_PROTOC})
    message(STATUS "Using vcpkg native protoc for code generation")
    
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
    
    # Custom command to generate protobuf files
    add_custom_command(
      OUTPUT ${PROTO_GENERATED_SRCS} ${PROTO_GENERATED_HDRS}
      COMMAND ${VCPKG_PROTOC}
              --proto_path=${NEVRCOMMON_DIR}/proto
              --cpp_out=${PROTO_BINARY_DIR}
              ${PROTO_FILES}
      DEPENDS ${PROTO_FILES}
      COMMENT "Generating protobuf files with native protoc"
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
    IMPORT_DIRS ${PROTO_IMPORT_DIRS}
  )
endif()

target_link_libraries(proto_nevr PUBLIC protobuf::libprotobuf)

target_include_directories(proto_nevr PUBLIC
  $<BUILD_INTERFACE:${PROTO_BINARY_DIR}>
)