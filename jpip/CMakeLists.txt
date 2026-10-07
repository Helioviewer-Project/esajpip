# JPIP requests, indexed targets and responses, independent of the server runtime.
add_library(jpip STATIC
    source/file_segment.cc
    index/coding_parameters.cc
    index/image_index.cc
    request/request.cc
    request/response_request.cc
    response/databin_server.cc
    response/databin_writer.cc)
target_include_directories(jpip PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(jpip PUBLIC jpeg2000)
