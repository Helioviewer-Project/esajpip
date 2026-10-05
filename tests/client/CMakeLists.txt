# Tests of the JPIP client (client/). Protocol, reconstruction and source
# tests link jpip to check the client against what the server writes.
#
#   client_cache         append, identical overlap, conflicts and completeness
#   client_jpp           bytes from jpip::DataBinWriter read back by the client
#   client_jpp_malformed damaged versions of those bytes, all rejected
#   client_reconstruct   the corpus, the transcoder's reference images and
#                        the merger's reference movie served by
#                        jpip::DataBinServer, stored, written back as
#                        codestreams and, the images, decoded; the movie's
#                        frame count, XML and color tables

add_custom_target(client_tests)

function(client_add_test target name)
    cmake_parse_arguments(ARG "" "TIMEOUT" "SOURCES;LIBRARIES" ${ARGN})
    add_executable(${target} ${ARG_SOURCES})
    esajpip_configure_target(${target})
    target_link_libraries(${target} PRIVATE esajpip_client_wasm ${ARG_LIBRARIES})
    if(ARG_TIMEOUT)
        esajpip_add_test(${name} COMMAND ${target} LABELS client TIMEOUT ${ARG_TIMEOUT})
    else()
        esajpip_add_test(${name} COMMAND ${target} LABELS client)
    endif()
    add_dependencies(client_tests ${target})
endfunction()

client_add_test(test_client_cache client_cache SOURCES test_cache.c)
client_add_test(test_client_metadata client_metadata SOURCES test_metadata.cc)
client_add_test(test_client_image client_image SOURCES test_image.c)
client_add_test(test_client_jpp_roundtrip client_jpp SOURCES test_jpp_roundtrip.cc LIBRARIES jpip)
client_add_test(test_client_jpp_malformed client_jpp_malformed SOURCES test_jpp_malformed.cc LIBRARIES jpip)
client_add_test(test_client_reconstruct client_reconstruct SOURCES test_reconstruct.cc LIBRARIES jpip TIMEOUT 60)

target_compile_definitions(test_client_reconstruct PRIVATE
    VECTORS="${PROJECT_SOURCE_DIR}/tests/vectors/j2k"
    IMAGES="${PROJECT_SOURCE_DIR}/tests/transcode/fixtures/kakadu"
    MOVIE="${ESAJPIP_CLIENT_MOVIE_FIXTURE}")

client_add_test(test_client_source client_source SOURCES test_client.cc LIBRARIES jpip TIMEOUT 60)
target_compile_definitions(test_client_source PRIVATE
    GRAY="${ESAJPIP_CLIENT_GRAY_FIXTURE}"
    GRAY_LARGE="${PROJECT_SOURCE_DIR}/tests/transcode/fixtures/kakadu/solo_fsi174_509x513_PCRL.jp2"
    IMAGE="${ESAJPIP_CLIENT_RGB_FIXTURE}"
    MOVIE="${ESAJPIP_CLIENT_MOVIE_FIXTURE}")

# Local source access and selection require no built-in decoder.
add_executable(test_client_local test_local_source.c)
esajpip_configure_target(test_client_local)
target_link_libraries(test_client_local PRIVATE esajpip_client)
esajpip_add_test(client_local COMMAND test_client_local LABELS client)
add_dependencies(client_tests test_client_local)
target_compile_definitions(test_client_local PRIVATE
    IMAGE="${ESAJPIP_CLIENT_RGB_FIXTURE}"
    MOVIE="${ESAJPIP_CLIENT_MOVIE_FIXTURE}")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
esajpip_add_test(client_converter COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/check_converter.py $<TARGET_FILE:hvc_jpp2j2k>
    $<TARGET_FILE:test_client_source> LABELS client cli)
add_dependencies(client_tests hvc_jpp2j2k)
