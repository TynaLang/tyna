cmake_minimum_required(VERSION 3.15)
project(tyl C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")

if(CMAKE_EXPORT_COMPILE_COMMANDS)
  set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES
      ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})
endif()

# Find LLVM
find_package(LLVM REQUIRED CONFIG)
include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# Source files
file(GLOB_RECURSE SRC_FILES src/*.c)

add_executable(tyl ${SRC_FILES})

# Link LLVM (core + executionengine for JIT)
llvm_map_components_to_libnames(LLVM_LIBS
    core
    executionengine
    native
    mc
    mcjit
    orcjit
    support
)

# Apply LLVM includes / defs safely after target exists
target_include_directories(tyl PRIVATE ${LLVM_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/include/tyl)
target_compile_definitions(tyl PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(tyl PRIVATE ${LLVM_LIBS})

set_target_properties(tyl PROPERTIES LINKER_LANGUAGE CXX)

target_compile_options(tyl PRIVATE -Wno-unused-parameter -Wno-unused-function)
