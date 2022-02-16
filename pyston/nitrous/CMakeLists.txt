set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ferror-limit=5 -fcolor-diagnostics")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ferror-limit=5 -fcolor-diagnostics")

if("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64")
    set(LLVM_ARCH_DEP_LIBS LLVMX86CodeGen LLVMX86AsmParser)
elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "aarch64")
    set(LLVM_ARCH_DEP_LIBS LLVMAArch64CodeGen LLVMAArch64AsmParser)
else()
    message(FATAL_ERROR "Unknown arch: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(LLVM_LIB_DEPS LLVMCore LLVMSupport LLVMBitReader LLVMAsmParser LLVMTransformUtils LLVMScalarOpts  LLVMOrcJIT ${LLVM_ARCH_DEP_LIBS})

add_library(interp SHARED interp.cpp jit.cpp symbol_finder.cpp Execution.cpp Interpreter.cpp facts.cpp)
target_include_directories(interp PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(interp ${LLVM_LIB_DEPS} "ffi")
