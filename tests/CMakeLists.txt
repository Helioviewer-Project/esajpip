# Every test of the project, in one place: jpeg2000 (jpeg2000/), hv_transcode
# and hv_merge, their command lines, the server, and the libFuzzer targets.
# The component CMakeLists.txt files build libraries and tools only.
#
#   cmake -S . -B build -DBUILD_TESTING=ON
#   ctest --test-dir build -L tools          jpeg2000, hv_transcode, hv_merge
#   ctest --test-dir build -L jpip           JPIP library tests
#   ctest --test-dir build -L server         the server's
#   ctest --test-dir build -L fuzz           the libFuzzer smoke runs
#   tests/run.sh [normal|sanitize] [CTest options]
#
# Groups: jpeg2000/ (the reader, writer, rewrite path, geometry, hv_file,
# hv_served), jpip/ (requests, index and response engine), transcode/, merge/, server/, fuzz/.

# The two switches of the suite. Everything else about the tests -- the
# helper, the labels, the targets -- is below or in the group directories.
option(BUILD_TESTING "Build and run the tests" OFF)
option(ESAJPIP_FUZZ "Build the libFuzzer targets in tests/fuzz" OFF)

if(NOT BUILD_TESTING)
    return()
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# A sanitizer report fails every test, and every test is registered through
# this one helper, so labels and environment cannot drift between groups.
set(ESAJPIP_TEST_ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
set(ESAJPIP_CLIENT_RGB_FIXTURE
    ${PROJECT_SOURCE_DIR}/tests/transcode/fixtures/kakadu/synthetic_rgb_129x129_CPRL_SOP_EPH.jp2)
set(ESAJPIP_CLIENT_GRAY_FIXTURE
    ${PROJECT_SOURCE_DIR}/tests/transcode/fixtures/kakadu/solo_fsi174_127x129_RLCP_PLT.jp2)
set(ESAJPIP_CLIENT_MOVIE_FIXTURE
    ${PROJECT_SOURCE_DIR}/tests/merge/fixtures/expected/merged.jpx)

# esajpip_add_test(<name> COMMAND <command...> LABELS <label...>
#                  [WORKING_DIRECTORY <dir>] [TIMEOUT <seconds>])
function(esajpip_add_test name)
    cmake_parse_arguments(ARG "" "WORKING_DIRECTORY;TIMEOUT" "COMMAND;LABELS" ${ARGN})
    if(ARG_WORKING_DIRECTORY)
        add_test(NAME ${name} COMMAND ${ARG_COMMAND} WORKING_DIRECTORY ${ARG_WORKING_DIRECTORY})
    else()
        add_test(NAME ${name} COMMAND ${ARG_COMMAND})
    endif()
    set_tests_properties(${name} PROPERTIES
        LABELS "${ARG_LABELS}"
        ENVIRONMENT "${ESAJPIP_TEST_ENVIRONMENT}")
    if(ARG_TIMEOUT)
        # Preserve the mutation runner's explicit per-test deadline.
        if(DEFINED ESAJPIP_TEST_TIMEOUT)
            set(ARG_TIMEOUT "${ESAJPIP_TEST_TIMEOUT}")
        endif()
        set_tests_properties(${name} PROPERTIES TIMEOUT "${ARG_TIMEOUT}")
    endif()
endfunction()

function(esajpip_client_response_fixtures target)
    target_compile_definitions(${target} PRIVATE
        CLIENT_RGB="${ESAJPIP_CLIENT_RGB_FIXTURE}"
        CLIENT_GRAY="${ESAJPIP_CLIENT_GRAY_FIXTURE}"
        CLIENT_MOVIE="${ESAJPIP_CLIENT_MOVIE_FIXTURE}")
endfunction()

# The model's own checks that need no compiler (spec/check-model.sh --static).
esajpip_add_test(model_static
    COMMAND sh ${PROJECT_SOURCE_DIR}/spec/check-model.sh --static
    LABELS tools model)

add_subdirectory(jpeg2000)
add_subdirectory(jpip)
add_subdirectory(transcode)
add_subdirectory(merge)
add_subdirectory(server)
add_subdirectory(client)

# The fuzz targets and their deterministic replay driver. The driver builds
# in every configuration; the targets themselves are gated by ESAJPIP_FUZZ
# inside.
add_subdirectory(fuzz)

esajpip_add_test(standards_regression
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/jpeg2000/standards_test.py
        $<TARGET_FILE:hv_walk> $<TARGET_FILE:hv_merge> $<TARGET_FILE:hv_transcode>
        ${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/standards
    LABELS tools)
esajpip_add_test(mutation_runner
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_mutation.py
    LABELS infrastructure)
