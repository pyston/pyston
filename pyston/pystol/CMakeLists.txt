set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ferror-limit=5 -fcolor-diagnostics")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ferror-limit=5 -fcolor-diagnostics")

add_library(pystol SHARED pystol.cpp recursion_check_removal.cpp passes.cpp)
target_include_directories(pystol PRIVATE ${CMAKE_SOURCE_DIR}/../build/bc_install/usr/include/python3.8-pyston2.3 ${CMAKE_SOURCE_DIR}/nitrous)
target_include_directories(pystol PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(pystol interp)
