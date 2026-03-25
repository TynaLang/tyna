cmake_minimum_required(VERSION 3.15)
project(tyl C)
set(CMAKE_C_STANDARD 11)

# Find LLVM
find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# Source files
file(GLOB SRC_FILES src/*.c)

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
target_include_directories(tyl PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(tyl PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(tyl ${LLVM_LIBS})
