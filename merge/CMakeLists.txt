# hv_merge: the C port of hvJP2K's JPX merger, on the jpeg2000
# library in ../jpeg2000.

add_executable(hv_merge hv_merge.c merge.c)
target_sources(hv_merge PRIVATE ${PROJECT_SOURCE_DIR}/tools/hv_file.c)
target_link_libraries(hv_merge PRIVATE jpeg2000)

