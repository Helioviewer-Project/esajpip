# The fuzz targets of jpeg2000, JPIP, hv_transcode and hv_merge, their seed
# corpora, and their deterministic counterpart.
#
# `replay` calls the same target functions as libFuzzer, so the two cannot
# drift. It needs no libFuzzer runtime, is built always, and is what the
# sanitizer, coverage and Valgrind runs use:
#
#   build/tests-*/tests/fuzz/replay reader-rewrite FILE_OR_DIR...
#   build/tests-*/tests/fuzz/replay deferred-plt FILE_OR_DIR...
#   build/tests-*/tests/fuzz/replay asn1 FILE_OR_DIR...
#   build/tests-*/tests/fuzz/replay transcode-fuzz FILE_OR_DIR...
#   build/tests-*/tests/fuzz/replay merge-fuzz FILE_OR_DIR...
#
# The targets themselves need Clang's libFuzzer runtime (ESAJPIP_FUZZ=ON,
# checked in the top-level CMakeLists.txt). Each gets a short deterministic
# run in CTest, so a crash stays reproducible from an ordinary test run:
#
#   ctest --test-dir build -L fuzz

set(ESAJPIP_FUZZ_CORPUS_DIR ${CMAKE_CURRENT_BINARY_DIR}/corpus)
set(ESAJPIP_FUZZ_INCLUDES ${CMAKE_CURRENT_BINARY_DIR})
set(ESAJPIP_FUZZ_SOURCES
    ${PROJECT_SOURCE_DIR}/transcode/transcode.c
    ${PROJECT_SOURCE_DIR}/transcode/tier2.c
    ${PROJECT_SOURCE_DIR}/merge/merge.c)
set(ESAJPIP_JPIP_FUZZ_SOURCES
    fuzz_jpip_request.cc fuzz_jpip_writer.cc fuzz_jpip_cache.cc fuzz_jpip_session.cc)
set(ESAJPIP_CLIENT_FUZZ_SOURCES fuzz_client_response.cc fuzz_client_source.cc)
set(ESAJPIP_CLIENT_FIXTURES
    ${ESAJPIP_CLIENT_RGB_FIXTURE}
    ${ESAJPIP_CLIENT_GRAY_FIXTURE}
    ${ESAJPIP_CLIENT_MOVIE_FIXTURE})

include(corpus.cmake)
include(replay.cmake)

if(NOT ESAJPIP_FUZZ)
    # The replay driver above needs nothing else.
    return()
endif()

include(libfuzzer.cmake)
