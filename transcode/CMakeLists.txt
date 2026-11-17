# hv_transcode: the C port of hvJP2K's transcoder, on the jpeg2000
# library in ../jpeg2000.

add_library(hv_transcoder STATIC transcode.c transcode_file.c tier2.c)
target_link_libraries(hv_transcoder PUBLIC jpeg2000)

add_executable(hv_transcode hv_transcode.c)
target_sources(hv_transcode PRIVATE ${PROJECT_SOURCE_DIR}/tools/hv_file.c
    ${PROJECT_SOURCE_DIR}/tools/hv_cli.c)
target_link_libraries(hv_transcode PRIVATE hv_transcoder)

