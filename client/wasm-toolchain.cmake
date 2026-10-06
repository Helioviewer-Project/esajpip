# Configure in a separate directory with -DCMAKE_TOOLCHAIN_FILE=client/wasm-toolchain.cmake.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR wasm32)
find_program(ESAJPIP_ZIG zig REQUIRED)
set(CMAKE_C_COMPILER ${ESAJPIP_ZIG} cc -target wasm32-wasi)
set(CMAKE_CXX_COMPILER ${ESAJPIP_ZIG} c++ -target wasm32-wasi)
set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
