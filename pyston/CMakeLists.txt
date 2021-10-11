cmake_minimum_required(VERSION 3.1)
project(interp)

if("$ENV{PYSTON_USE_SYS_BINS}" STREQUAL "1")
    find_package(LLVM)
else()
    set(CMAKE_C_FLAGS_PARTIALDEBUG "${CMAKE_C_FLAGS_DEBUG}")
    set(CMAKE_CXX_FLAGS_PARTIALDEBUG "${CMAKE_CXX_FLAGS_DEBUG}")

    set(CMAKE_BUILD_TYPE_SAVED "${CMAKE_BUILD_TYPE}")
    if(${CMAKE_BUILD_TYPE} STREQUAL "PartialDebug")
        set(CMAKE_BUILD_TYPE Release)

        set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -UNDEBUG")
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -UNDEBUG")
    endif()
    set(LLVM_TARGETS_TO_BUILD "host" CACHE STRING "LLVM targets")

    add_subdirectory(${CMAKE_SOURCE_DIR}/llvm/llvm ${CMAKE_BINARY_DIR}/llvm EXCLUDE_FROM_ALL)
    set(CMAKE_BUILD_TYPE "${CMAKE_BUILD_TYPE_SAVED}")

    list(APPEND CMAKE_MODULE_PATH "${CMAKE_BINARY_DIR}/llvm/lib/cmake/llvm/")
    include(LLVMConfig)
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror -Wreturn-type -Wno-sign-compare -Wno-unused -Wno-unused-parameter")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS} -fno-rtti -std=c++14 -D__STDC_FORMAT_MACROS")

set(CLANG_FLAGS "-Qunused-arguments -fcolor-diagnostics" CACHE STRING "Clang specific C and CXX flags")
if("${CMAKE_C_COMPILER_ID}" STREQUAL "Clang")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CLANG_FLAGS}")
endif()
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CLANG_FLAGS}")
endif()

add_subdirectory(nitrous)
add_subdirectory(pystol)
