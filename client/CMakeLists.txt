# Core client for a host decoder; the extended client also decodes with OpenJPEG.
set(esajpip_client_srcs
    hvc.c hvc_jpp.c hvc_cache.c hvc_frame.c hvc_reconstruct.c hvc_metadata.c)

set(openjpeg_srcs
    bio.c cio.c dwt.c event.c function_list.c ht_dec.c image.c invert.c j2k.c
    jp2.c mct.c mqc.c openjpeg.c opj_malloc.c pi.c sparse_array.c t1.c t2.c
    tcd.c tgt.c thread.c)
list(TRANSFORM openjpeg_srcs PREPEND vendor/openjpeg/)
set_source_files_properties(${openjpeg_srcs} PROPERTIES COMPILE_OPTIONS -w)

set(esajpip_client_wasm_srcs
    ${esajpip_client_srcs} hvc_openjpeg.c hvc_wasm.c ${openjpeg_srcs})

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "wasm32")
    # WASI libc's allocator avoids Zig's power-of-two block rounding.
    add_executable(esajpip_client_wasm ${esajpip_client_wasm_srcs} vendor/dlmalloc/dlmalloc.c)
    target_link_libraries(esajpip_client_wasm PRIVATE jpeg2000)
    set_source_files_properties(vendor/dlmalloc/dlmalloc.c PROPERTIES COMPILE_OPTIONS -w)
    option(ESAJPIP_WASM_SIMD "Require WebAssembly SIMD for the client decoder" OFF)
    if(ESAJPIP_WASM_SIMD)
        target_compile_options(jpeg2000 PRIVATE -msimd128)
        target_compile_options(esajpip_client_wasm PRIVATE -msimd128)
        target_link_options(esajpip_client_wasm PRIVATE -msimd128)
    endif()
    target_link_options(esajpip_client_wasm PRIVATE
        -mexec-model=reactor -s -Wl,-z,stack-size=1048576)
    set(esajpip_web ${CMAKE_CURRENT_BINARY_DIR}/web)
    set_target_properties(esajpip_client_wasm PROPERTIES
        OUTPUT_NAME esajpip_client SUFFIX .wasm RUNTIME_OUTPUT_DIRECTORY ${esajpip_web})
    configure_file(vendor/openjpeg/NOTICES ${esajpip_web}/OpenJPEG-NOTICES.txt COPYONLY)
    configure_file(vendor/dlmalloc/LICENSE-MIT ${esajpip_web}/dlmalloc-LICENSE.txt COPYONLY)
    foreach(file js/jpip_source.mjs js/jpip_worker.mjs js/jpip_channel.mjs demo/index.html)
        get_filename_component(name ${file} NAME)
        configure_file(${file} ${esajpip_web}/${name} COPYONLY)
    endforeach()
else()
    add_library(esajpip_client STATIC ${esajpip_client_srcs})
    target_include_directories(esajpip_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(esajpip_client PUBLIC jpeg2000 PRIVATE m)

    # Native decoding tests compile the same extended source list as WebAssembly.
    add_library(esajpip_client_wasm STATIC EXCLUDE_FROM_ALL ${esajpip_client_wasm_srcs})
    target_link_libraries(esajpip_client_wasm PUBLIC jpeg2000 PRIVATE m)

    if(NOT ESAJPIP_CLIENT_ONLY)
        add_executable(hvc_jpp2j2k hvc_jpp2j2k.c ${PROJECT_SOURCE_DIR}/tools/hv_file.c)
        target_link_libraries(hvc_jpp2j2k PRIVATE esajpip_client)
    endif()
endif()

target_include_directories(esajpip_client_wasm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(esajpip_client_wasm SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/openjpeg)
