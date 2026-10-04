# Tests of the JPIP client (client/). Protocol, reconstruction and source
# tests link jpip to check the client against what the server writes.
#
#   client_cache         the store's append and completeness rules
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
    IMAGE="${ESAJPIP_CLIENT_RGB_FIXTURE}"
    MOVIE="${ESAJPIP_CLIENT_MOVIE_FIXTURE}")
