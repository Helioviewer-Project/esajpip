# Every test here links only the JPIP library and its C format dependency.
# Build the complete library suite without building the server or tools.
add_custom_target(jpip_tests)

function(jpip_add_test target name group source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE jpip)
    esajpip_add_test(${name} COMMAND ${target} LABELS tools jpip jpip_${group} TIMEOUT 30)
    add_dependencies(jpip_tests ${target})
endfunction()

jpip_add_test(test_jpip_source jpip_source source source/test_source.cc)
jpip_add_test(test_index index_library index index/test_index.cc)
jpip_add_test(test_jpip_packet_layout jpip_packet_layout index index/test_packet_layout.cc)
jpip_add_test(test_request jpip_request request request/test_request.cc)
jpip_add_test(test_jpip_cache jpip_cache response response/test_cache.cc)
jpip_add_test(test_jpip_window jpip_window response response/test_window.cc)
jpip_add_test(test_jpip_writer jpip_writer response response/test_writer.cc)
jpip_add_test(test_jpip jpip_library response response/test_session.cc)

# Only the corpus/index and end-to-end memory-session tests read these fixtures.
foreach(target test_index test_jpip)
    target_compile_definitions(${target} PRIVATE
        VECTORS="${PROJECT_SOURCE_DIR}/tests/vectors/j2k")
endforeach()
