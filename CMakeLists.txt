cmake_minimum_required(VERSION 3.15)
project(tyna LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS src/*.c)

add_executable(tyna ${SRC_FILES})

target_include_directories(tyna
    PRIVATE
        ${LLVM_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(tyna
    PRIVATE
        ${LLVM_DEFINITIONS}
)

if(TARGET LLVM::LLVM)
    target_link_libraries(tyna PRIVATE LLVM::LLVM)
else()
    target_link_libraries(tyna PRIVATE LLVM)
endif()

target_compile_options(tyna
    PRIVATE
        -Wno-unused-parameter
        -Wno-unused-function
)

set_target_properties(tyna PROPERTIES LINKER_LANGUAGE CXX)
set_target_properties(tyna PROPERTIES ENABLE_EXPORTS ON)
target_link_options(tyna PRIVATE -rdynamic)
target_link_libraries(tyna PRIVATE dl)
