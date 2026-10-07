cmake_minimum_required(VERSION 3.20)

project(esajpip LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

option(ESAJPIP_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ESAJPIP_CLIENT_ONLY "Build the client without the server or tests" OFF)

if(ESAJPIP_SANITIZE AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "ESAJPIP_SANITIZE requires Clang or GCC")
endif()

# Use LTO for optimized builds when supported. Explicit CMake IPO settings
# take precedence; keep the default Debug and sanitizer builds without LTO.
if(NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION AND NOT ESAJPIP_SANITIZE)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ESAJPIP_IPO_SUPPORTED LANGUAGES C CXX)
    if(ESAJPIP_IPO_SUPPORTED)
        foreach(config RELEASE RELWITHDEBINFO MINSIZEREL)
            if(NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION_${config})
                set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_${config} ON)
            endif()
        endforeach()
    else()
        message(STATUS "LTO is not supported by this toolchain; building without it")
    endif()
endif()

# Build policy for this source tree, inherited by its targets and subdirectories.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -Wno-unused-parameter
    "$<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>"
    "$<$<CONFIG:RelWithDebInfo>:-fno-omit-frame-pointer>")
if(ESAJPIP_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -UNDEBUG)
    add_link_options(-fsanitize=address,undefined)
endif()

# Native client dependencies do not require the server libraries.
add_subdirectory(jpeg2000)
add_subdirectory(client)
if(ESAJPIP_CLIENT_ONLY OR CMAKE_SYSTEM_PROCESSOR STREQUAL "wasm32")
    return()
endif()

set(ESAJPIP_IMAGE_DIRECTORY "images" CACHE STRING
    "Image directory written to server.ini")
set(ESAJPIP_LOG_DIRECTORY "" CACHE STRING
    "Log directory written to server.ini; empty uses standard output")
if(ESAJPIP_LOG_DIRECTORY)
    set(ESAJPIP_FILE_LOGGING true)
else()
    set(ESAJPIP_FILE_LOGGING false)
endif()
configure_file(server.ini ${CMAKE_CURRENT_BINARY_DIR}/server.ini)

# Format tools and server runtime.
add_subdirectory(jpip)
add_subdirectory(transcode)
add_subdirectory(merge)
add_subdirectory(server)

# Explicit benchmark target; excluded from normal builds and installation.
add_executable(jpip_bench EXCLUDE_FROM_ALL tools/jpip_bench.cc)
target_link_libraries(jpip_bench PRIVATE esajpip_server)

install(TARGETS esajpip RUNTIME DESTINATION bin)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/server.ini DESTINATION bin)

# Tests: tests/ owns the gate, the options, the helpers and every target,
# including the fuzz ones. enable_testing() is here because CTest reads the
# top-level CTestTestfile.cmake.
enable_testing()
add_subdirectory(tests)
