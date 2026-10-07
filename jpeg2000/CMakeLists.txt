# jpeg2000: the JPEG 2000 reader/writer, for hv_transcode, hv_merge and the
# server. hv_reader, hv_rules, hv_writer, hv_rewrite and hv_geometry are hand-written; generated/ is produced
# from the model in ../spec by generate.sh (asn1scc is not part of the
# build).

# The generated modules are those of ../spec/modules, the one list that
# generate.sh and ../spec/check-model.sh read too.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/../spec/modules jpeg2000_modules
    REGEX "^[a-z0-9-]+$")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/../spec/modules)
set(jpeg2000_generated
    generated/asn1crt.c
    generated/asn1crt_encoding.c
    generated/asn1crt_encoding_acn.c
    generated/asn1crt_encoding_uper.c)
foreach(module ${jpeg2000_modules})
    list(APPEND jpeg2000_generated generated/${module}.c)
endforeach()

add_library(jpeg2000 STATIC
    hv_reader.c
    hv_render.c
    hv_metadata.c
    hv_rules.c
    hv_mapping.c
    hv_error.c
    ${jpeg2000_generated})
target_include_directories(jpeg2000 PUBLIC
    ${PROJECT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/generated)

# WASM uses the in-memory reader; native callers also read files and write JP2/JPX.
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "wasm32")
    return()
endif()
target_sources(jpeg2000 PRIVATE hv_local.c hv_writer.c hv_rewrite.c hv_geometry.c)

# Filesystem checks of the served profile and its CLI are not client code.
if(NOT ESAJPIP_CLIENT_ONLY)
    target_sources(jpeg2000 PRIVATE hv_served.c)
    # hv_walk checks files with the reader and rewrites them with -w.
    add_executable(hv_walk hv_walk.c)
    target_link_libraries(hv_walk PRIVATE jpeg2000)
endif()
