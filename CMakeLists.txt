cmake_minimum_required(VERSION 3.20)
project(subprocess_test CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_executable(subprocess_test
    main.cpp
    PythonSubprocess.hpp
    JsonIpcChannel.hpp
)

target_include_directories(subprocess_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Create a symlink in the build directory pointing to the source worker.py
file(CREATE_LINK
    "${CMAKE_CURRENT_SOURCE_DIR}/worker.py"
    "${CMAKE_CURRENT_BINARY_DIR}/worker.py"
    SYMBOLIC
)

# Optional: find nlohmann_json if installed system-wide
find_package(nlohmann_json QUIET)
if(nlohmann_json_FOUND)
    target_link_libraries(subprocess_test PRIVATE nlohmann_json::nlohmann_json)
endif()