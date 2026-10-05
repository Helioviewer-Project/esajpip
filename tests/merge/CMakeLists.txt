# hv_merge's tests: the library of merge/, the command line, and the merge
# shapes the corpus carries (the transcoder's fixtures are read too).

set(MERGE_TRANSCODE_FIXTURES ${PROJECT_SOURCE_DIR}/tests/transcode/fixtures)

add_executable(test_merge test_merge.c ${PROJECT_SOURCE_DIR}/merge/merge.c)
esajpip_configure_target(test_merge)
target_compile_definitions(test_merge PRIVATE
    JPEG2000_VECTORS="${PROJECT_SOURCE_DIR}/tests/vectors/j2k"
    MERGE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    TRANSCODE_FIXTURES="${MERGE_TRANSCODE_FIXTURES}")
target_link_libraries(test_merge PRIVATE jpeg2000)
esajpip_add_test(merge COMMAND test_merge LABELS tools)

esajpip_add_test(merge_command
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/cli_test.sh
        $<TARGET_FILE:hv_merge>
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
        ${MERGE_TRANSCODE_FIXTURES}
        ${CMAKE_CURRENT_BINARY_DIR}/command
    LABELS tools cli)
