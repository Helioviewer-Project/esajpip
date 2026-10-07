# Adapters and lifecycle tests; library-only algorithms live in tests/jpip.
add_custom_target(server_tests)

function(server_add_test target name group source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE esajpip_server)
    if(group STREQUAL "live")
        set(timeout 90)
    else()
        set(timeout 30)
    endif()
    esajpip_add_test(${name} COMMAND ${target} LABELS server server_${group} TIMEOUT ${timeout})
    if(group STREQUAL "live")
        get_test_property(${name} TIMEOUT timeout)
        target_compile_definitions(${target} PRIVATE ESAJPIP_LIVE_TEST_TIMEOUT=${timeout})
    endif()
    add_dependencies(server_tests ${target})
endfunction()

add_executable(logging_test logging_test.cc ${PROJECT_SOURCE_DIR}/server/trace.cc)
target_compile_definitions(logging_test PRIVATE
    ESAJPIP_LOG_LIMIT=1024
    ESAJPIP_LOG_QUEUE_LIMIT=4096)
target_include_directories(logging_test PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(logging_test PRIVATE Threads::Threads PkgConfig::PKG)
esajpip_add_test(logging COMMAND logging_test LABELS server server_support TIMEOUT 30)
add_dependencies(server_tests logging_test)

server_add_test(support_test server_support support support_test.cc)
server_add_test(file_manager_test server_storage storage file_manager_test.cc)
target_compile_definitions(file_manager_test PRIVATE
    JPEG2000_VECTOR_DIRECTORY="${PROJECT_SOURCE_DIR}/tests/vectors/j2k")
server_add_test(channel_engine_test server_engine channel channel_engine_test.cc)
server_add_test(server_overlap_test server_overlap live server_overlap_test.cc)
foreach(target channel_engine_test server_overlap_test)
    target_compile_definitions(${target} PRIVATE
        VECTORS="${PROJECT_SOURCE_DIR}/tests/vectors/j2k")
endforeach()
server_add_test(request_head_test server_http http request_head_test.cc)
server_add_test(server_connection_test server_connection http server_connection_test.cc)
server_add_test(server_test server live server_test.cc)
set_tests_properties(server server_overlap server_connection PROPERTIES RESOURCE_LOCK server_ports)
