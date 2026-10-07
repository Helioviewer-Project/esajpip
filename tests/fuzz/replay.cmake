# The deterministic driver: no libFuzzer, so it builds in any configuration.
add_executable(replay replay.c ${ESAJPIP_FUZZ_SOURCES} ${ESAJPIP_JPIP_FUZZ_SOURCES} ${ESAJPIP_CLIENT_FUZZ_SOURCES})
target_include_directories(replay PRIVATE ${ESAJPIP_FUZZ_INCLUDES})
target_compile_definitions(replay PRIVATE ESAJPIP_FUZZ_REPLAY)
target_link_libraries(replay PRIVATE jpip esajpip_client_wasm)
esajpip_client_response_fixtures(replay)
add_dependencies(replay client_fuzz_corpus)

foreach(area response source)
    esajpip_add_test(fuzz_replay_client_${area}
        COMMAND replay client-${area} ${ESAJPIP_FUZZ_CORPUS_DIR}/client-${area}
        LABELS fuzz replay client_fuzz TIMEOUT 60)
endforeach()

if(TARGET fuzz_corpus)
    add_dependencies(replay fuzz_corpus)

    function(fuzz_replay_test name corpus)
        esajpip_add_test(fuzz_replay_${name}
            COMMAND replay ${corpus} ${ESAJPIP_FUZZ_CORPUS_DIR}/${corpus}
            LABELS fuzz replay)
    endfunction()

    fuzz_replay_test(reader_rewrite reader-rewrite)
    fuzz_replay_test(deferred_plt deferred-plt)
    fuzz_replay_test(asn1 asn1)
    fuzz_replay_test(transcode transcode-fuzz)
    fuzz_replay_test(merge merge-fuzz)

    foreach(area request writer cache session)
        esajpip_add_test(fuzz_replay_jpip_${area}
            COMMAND replay jpip-${area} ${ESAJPIP_FUZZ_CORPUS_DIR}/jpip-${area}
            LABELS fuzz replay jpip_fuzz TIMEOUT 30)
    endforeach()
endif()
