# hv_transcode's tests: the library of transcode/, and the command line.

add_executable(test_transcode test_transcode.c
    ${PROJECT_SOURCE_DIR}/transcode/transcode_file.c
    ${PROJECT_SOURCE_DIR}/transcode/transcode.c
    ${PROJECT_SOURCE_DIR}/transcode/tier2.c)
target_compile_definitions(test_transcode PRIVATE
    TRANSCODE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_link_libraries(test_transcode PRIVATE jpeg2000)
esajpip_add_test(transcode COMMAND test_transcode LABELS tools)

esajpip_add_test(transcode_command
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/cli_test.sh
        $<TARGET_FILE:hv_transcode>
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
        ${CMAKE_CURRENT_BINARY_DIR}/command
    LABELS tools cli)
