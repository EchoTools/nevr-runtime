# Luau binding for the script host spike (#440). Builds the pieces of upstream
# Luau (extern/luau) a runtime that compiles and runs source needs, from our own
# CMake so that nothing else of upstream (CLI, Analysis, CodeGen, Config, Require,
# tests) is configured or built:
#
#   Luau.Common   Common/src          (assert/flag plumbing the others include)
#   Luau.Ast      Ast/src             (lexer and parser; throws ParseError)
#   Luau.Bytecode Bytecode/src        (bytecode builder; the compiler's backend)
#   Luau.Compiler Compiler/src        (luau_compile: source -> bytecode)
#   Luau.VM       VM/src              (the interpreter and its standard libraries)
#
# Error handling split (see luau_vm.cpp for the reasons):
#   - the VM is built with LUA_USE_LONGJMP=1 and -fno-exceptions: errors inside
#     the VM never use C++ exceptions, so nothing C++ unwinds through extern "C".
#   - Ast and Compiler are built WITH exceptions: the parser throws ParseError
#     and the compiler throws CompileError, both caught inside luau_compile.
#     Upstream gives this code no non-throwing mode.
set(NEVR_LUAU_TAG "0.742")  # must equal the tag extern/luau is pinned to
get_filename_component(NEVR_LUAU_DIR "${NEVR_SRC_DIR}/../extern/luau" ABSOLUTE)
if(NOT EXISTS "${NEVR_LUAU_DIR}/VM/src/lapi.cpp")
  message(FATAL_ERROR "extern/luau is empty; run: git submodule update --init extern/luau")
endif()

# Upstream's own warning set (CMakeLists.txt LUAU_OPTIONS), without -Werror.
set(NEVR_LUAU_WARNINGS -Wall -Wimplicit-fallthrough -Wsign-compare)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  list(APPEND NEVR_LUAU_WARNINGS -Wno-unused -Wno-maybe-uninitialized)
endif()

function(nevr_luau_lib name)
  add_library(${name} STATIC ${ARGN})
  target_compile_options(${name} PRIVATE ${NEVR_LUAU_WARNINGS})
endfunction()

nevr_luau_lib(nevr_luau_common
  "${NEVR_LUAU_DIR}/Common/src/BytecodeWire.cpp"
  "${NEVR_LUAU_DIR}/Common/src/StringUtils.cpp"
  "${NEVR_LUAU_DIR}/Common/src/TimeTrace.cpp")
target_include_directories(nevr_luau_common PUBLIC "${NEVR_LUAU_DIR}/Common/include")

nevr_luau_lib(nevr_luau_ast
  "${NEVR_LUAU_DIR}/Ast/src/Allocator.cpp"
  "${NEVR_LUAU_DIR}/Ast/src/Ast.cpp"
  "${NEVR_LUAU_DIR}/Ast/src/Confusables.cpp"
  "${NEVR_LUAU_DIR}/Ast/src/Cst.cpp"
  "${NEVR_LUAU_DIR}/Ast/src/Lexer.cpp"
  "${NEVR_LUAU_DIR}/Ast/src/Location.cpp"
  "${NEVR_LUAU_DIR}/Ast/src/Parser.cpp"
  "${NEVR_LUAU_DIR}/Ast/src/PrettyPrinter.cpp")
target_include_directories(nevr_luau_ast PUBLIC "${NEVR_LUAU_DIR}/Ast/include")
target_link_libraries(nevr_luau_ast PUBLIC nevr_luau_common)

nevr_luau_lib(nevr_luau_bytecode
  "${NEVR_LUAU_DIR}/Bytecode/src/BytecodeBuilder.cpp"
  "${NEVR_LUAU_DIR}/Bytecode/src/BytecodeDump.cpp"
  "${NEVR_LUAU_DIR}/Bytecode/src/BytecodeGraph.cpp"
  "${NEVR_LUAU_DIR}/Bytecode/src/Sccp.cpp")
target_include_directories(nevr_luau_bytecode PUBLIC "${NEVR_LUAU_DIR}/Bytecode/include")
target_link_libraries(nevr_luau_bytecode PUBLIC nevr_luau_common)

nevr_luau_lib(nevr_luau_compiler
  "${NEVR_LUAU_DIR}/Compiler/src/Compiler.cpp"
  "${NEVR_LUAU_DIR}/Compiler/src/Builtins.cpp"
  "${NEVR_LUAU_DIR}/Compiler/src/BuiltinFolding.cpp"
  "${NEVR_LUAU_DIR}/Compiler/src/ConstantFolding.cpp"
  "${NEVR_LUAU_DIR}/Compiler/src/CostModel.cpp"
  "${NEVR_LUAU_DIR}/Compiler/src/TableShape.cpp"
  "${NEVR_LUAU_DIR}/Compiler/src/Types.cpp"
  "${NEVR_LUAU_DIR}/Compiler/src/ValueTracking.cpp"
  "${NEVR_LUAU_DIR}/Compiler/src/lcode.cpp")
target_include_directories(nevr_luau_compiler PUBLIC "${NEVR_LUAU_DIR}/Compiler/include")
target_compile_definitions(nevr_luau_compiler PUBLIC "LUACODE_API=extern \"C\"")
target_link_libraries(nevr_luau_compiler PUBLIC nevr_luau_ast nevr_luau_bytecode)

nevr_luau_lib(nevr_luau_vm
  "${NEVR_LUAU_DIR}/VM/src/lapi.cpp"
  "${NEVR_LUAU_DIR}/VM/src/laux.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lbaselib.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lbitlib.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lbuffer.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lbuflib.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lbuiltins.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lcorolib.cpp"
  "${NEVR_LUAU_DIR}/VM/src/ldblib.cpp"
  "${NEVR_LUAU_DIR}/VM/src/ldebug.cpp"
  "${NEVR_LUAU_DIR}/VM/src/ldo.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lfunc.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lgc.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lgcdebug.cpp"
  "${NEVR_LUAU_DIR}/VM/src/linit.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lmathlib.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lmem.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lnumprint.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lobject.cpp"
  "${NEVR_LUAU_DIR}/VM/src/loslib.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lperf.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lstate.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lstring.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lstrlib.cpp"
  "${NEVR_LUAU_DIR}/VM/src/ltable.cpp"
  "${NEVR_LUAU_DIR}/VM/src/ltablib.cpp"
  "${NEVR_LUAU_DIR}/VM/src/ltm.cpp"
  "${NEVR_LUAU_DIR}/VM/src/ludata.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lutf8lib.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lveclib.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lintlib.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lvmexecute.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lclass.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lclasslib.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lvector.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lvmload.cpp"
  "${NEVR_LUAU_DIR}/VM/src/lvmutils.cpp")
target_include_directories(nevr_luau_vm PUBLIC "${NEVR_LUAU_DIR}/VM/include")
# Errors are longjmp, not C++ exceptions: luaconf.h LUA_USE_LONGJMP, ldo.cpp:12-70.
target_compile_definitions(nevr_luau_vm PUBLIC LUA_USE_LONGJMP=1 "LUA_API=extern \"C\"")
target_compile_options(nevr_luau_vm PRIVATE -fno-exceptions -fno-math-errno)
target_link_libraries(nevr_luau_vm PUBLIC nevr_luau_common)

add_library(nevr_script_vm STATIC luau_vm.cpp)
target_link_libraries(nevr_script_vm PUBLIC nevr_script_host
                      PRIVATE nevr_luau_compiler nevr_luau_vm)
target_compile_definitions(nevr_script_vm PRIVATE "NEVR_LUAU_TAG=\"${NEVR_LUAU_TAG}\"")
target_compile_options(nevr_script_vm PRIVATE ${NEVR_WARNINGS} -fno-exceptions)
