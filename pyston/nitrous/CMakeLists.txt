set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ferror-limit=5 -fcolor-diagnostics")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ferror-limit=5 -fcolor-diagnostics")

set(LLVM_LIB_DEPS LLVMCore LLVMSupport  LLVMBitReader LLVMAsmParser  LLVMTransformUtils LLVMScalarOpts  LLVMOrcJIT LLVMAArch64CodeGen LLVMAArch64AsmParser)

add_library(interp SHARED interp.cpp jit.cpp symbol_finder.cpp Execution.cpp Interpreter.cpp facts.cpp)
target_include_directories(interp PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(interp ${LLVM_LIB_DEPS} "ffi")
