# Only the server uses threads, GLib, llhttp and zlib. The targets are global
# because the tests and tools that link esajpip_server are defined in other
# directories.
find_package(Threads REQUIRED)
set_target_properties(Threads::Threads PROPERTIES IMPORTED_GLOBAL TRUE)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PKG REQUIRED IMPORTED_TARGET GLOBAL glib-2.0>=2.36)
pkg_check_modules(LLHTTP REQUIRED IMPORTED_TARGET GLOBAL libllhttp)
find_package(ZLIB REQUIRED)
set_target_properties(ZLIB::ZLIB PROPERTIES IMPORTED_GLOBAL TRUE)

# Vendored header-only Asio; see vendor/asio/README.md. A system include
# directory, so that the project's warning flags do not apply to it. Public
# because http/connection.h names Asio types.
add_library(esajpip_asio INTERFACE)
target_include_directories(esajpip_asio SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/asio/include)
target_compile_definitions(esajpip_asio INTERFACE ASIO_NO_DEPRECATED)

# Server runtime and filesystem adapter.
add_library(esajpip_server STATIC
    config.cc
    storage/file_manager.cc
    channel_engine.cc
    http/connection.cc
    http/request_head.cc
    server.cc
    trace.cc)
target_include_directories(esajpip_server PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(esajpip_server PUBLIC
    jpip
    PkgConfig::PKG
    PkgConfig::LLHTTP
    esajpip_asio
    ZLIB::ZLIB
    Threads::Threads)

add_executable(esajpip main.cc)
target_link_libraries(esajpip PRIVATE esajpip_server)
# Keep the executable beside the configured server.ini in the build root.
set_target_properties(esajpip PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
