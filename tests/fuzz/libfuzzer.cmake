# libFuzzer is a Clang component of its own: Homebrew's LLVM has it, the
# Apple Command Line Tools do not. One message here beats a linker error per
# target.
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "ESAJPIP_FUZZ requires Clang")
endif()
set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=fuzzer")
check_c_source_compiles(
    "int LLVMFuzzerTestOneInput(const unsigned char *d, unsigned long n)
     { (void)d; (void)n; return 0; }"
    ESAJPIP_LIBFUZZER_SUPPORTED)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
unset(CMAKE_REQUIRED_FLAGS)
if(NOT ESAJPIP_LIBFUZZER_SUPPORTED)
    message(FATAL_ERROR
        "ESAJPIP_FUZZ needs the libFuzzer runtime, which ${CMAKE_C_COMPILER} does\n"
        "not provide. Homebrew's LLVM does:\n"
        "  sh tests/run_profile.sh fuzz\n"
        "The replay driver needs none of it: build/tests/fuzz/replay")
endif()

# Instrument the library too, so the targets' coverage sees into it, and give
# everything that links it the coverage runtime.
target_compile_options(jpeg2000 PRIVATE -fsanitize=fuzzer-no-link -fno-sanitize-recover=all)
target_link_options(jpeg2000 INTERFACE -fsanitize=fuzzer-no-link)
target_compile_options(esajpip_client PRIVATE -fsanitize=fuzzer-no-link -fno-sanitize-recover=all)
target_link_options(esajpip_client INTERFACE -fsanitize=fuzzer-no-link)
target_compile_options(esajpip_client_wasm PRIVATE -fsanitize=fuzzer-no-link -fno-sanitize-recover=all)
target_link_options(esajpip_client_wasm INTERFACE -fsanitize=fuzzer-no-link)
target_compile_options(jpip PRIVATE -fsanitize=fuzzer-no-link -fno-sanitize-recover=all)
target_link_options(jpip INTERFACE -fsanitize=fuzzer-no-link)

# esajpip_fuzz_target(<name> <corpus> <sources...>): the shared part of a target.
function(esajpip_fuzz_target name corpus)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${ESAJPIP_FUZZ_INCLUDES})
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_compile_options(${name} PRIVATE -fno-sanitize-recover=all)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    set(fuzz_labels fuzz)
    set(fuzz_timeout)
    set(fuzz_options)
    if(name MATCHES "^fuzz_client_")
        target_link_libraries(${name} PRIVATE esajpip_client_wasm jpip)
        esajpip_client_response_fixtures(${name})
        add_dependencies(${name} client_fuzz_corpus)
        esajpip_add_test(fuzz_smoke_${name}
            COMMAND ${name} -runs=1000 -seed=1 -max_len=262144
                    ${ESAJPIP_FUZZ_CORPUS_DIR}/${corpus}
            LABELS fuzz client_fuzz TIMEOUT 60)
        return()
    elseif(name MATCHES "^fuzz_jpip_")
        target_link_libraries(${name} PRIVATE jpip)
        list(APPEND fuzz_labels jpip_fuzz)
        set(fuzz_timeout TIMEOUT 60)
    else()
        target_link_libraries(${name} PRIVATE jpeg2000)
    endif()
    if(name STREQUAL "fuzz_jpip_request")
        list(APPEND fuzz_options -dict=${CMAKE_CURRENT_SOURCE_DIR}/jpip.dict)
    endif()
    if(TARGET fuzz_corpus)
        add_dependencies(${name} fuzz_corpus)
        esajpip_add_test(fuzz_smoke_${name}
            COMMAND ${name} -runs=20000 -seed=1 -max_len=4096 ${fuzz_options}
                    ${ESAJPIP_FUZZ_CORPUS_DIR}/${corpus}
            LABELS ${fuzz_labels} ${fuzz_timeout})
    else()
        esajpip_add_test(fuzz_smoke_${name}
            COMMAND ${name} -runs=20000 -seed=1 -max_len=4096 ${fuzz_options}
            LABELS ${fuzz_labels} ${fuzz_timeout})
    endif()
endfunction()

# The library: the container reader and writer, deferred PLT consumption, and
# the generated ASN.1 decoders.
esajpip_fuzz_target(fuzz_reader_rewrite reader-rewrite fuzz_reader_rewrite.c)
esajpip_fuzz_target(fuzz_deferred_plt deferred-plt fuzz_deferred_plt.c)
esajpip_fuzz_target(fuzz_asn1 asn1 fuzz_asn1.c)

# The tools: a transcoded codestream is idempotent and in the served profile,
# and a merged file reads back.
esajpip_fuzz_target(fuzz_transcode transcode-fuzz fuzz_transcode.c
    ${PROJECT_SOURCE_DIR}/transcode/transcode.c
    ${PROJECT_SOURCE_DIR}/transcode/tier2.c)
esajpip_fuzz_target(fuzz_merge merge-fuzz fuzz_merge.c
    ${PROJECT_SOURCE_DIR}/merge/merge.c)

# JPIP API sequences and independent semantic checks, without the server runtime.
foreach(area request writer cache session)
    esajpip_fuzz_target(fuzz_jpip_${area} jpip-${area} fuzz_jpip_${area}.cc)
endforeach()
add_custom_target(jpip_fuzz DEPENDS replay
    fuzz_jpip_request fuzz_jpip_writer fuzz_jpip_cache fuzz_jpip_session)

foreach(area response source)
    esajpip_fuzz_target(fuzz_client_${area} client-${area} fuzz_client_${area}.cc)
endforeach()
add_custom_target(client_fuzz DEPENDS replay fuzz_client_response fuzz_client_source)
