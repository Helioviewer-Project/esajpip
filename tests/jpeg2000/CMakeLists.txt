# jpeg2000's tests: the reader, the rules, the writer, the rewrite path,
# hv_file, hv_served and hv_walk. The corpus they share is tests/vectors/j2k.

# The reader against the corpus: every vector the manifest calls
# standard-valid is accepted, and the profile label is the served profile's.
add_executable(test_profile test_profile.c)
esajpip_configure_target(test_profile)
target_link_libraries(test_profile PRIVATE jpeg2000)
esajpip_add_test(reader_profile
    COMMAND test_profile ${PROJECT_SOURCE_DIR}/tests/vectors/j2k
    LABELS tools)

# The writer against the corpus: every vector the reader accepts, written
# again from what it decoded (hv_rewrite).
add_executable(test_rewrite test_rewrite.c)
esajpip_configure_target(test_rewrite)
target_link_libraries(test_rewrite PRIVATE jpeg2000)
esajpip_add_test(writer_corpus
    COMMAND test_rewrite ${PROJECT_SOURCE_DIR}/tests/vectors/j2k
    LABELS tools)

# hv_file: replacing the tools' output files atomically.
add_executable(test_file test_file.c)
esajpip_configure_target(test_file)
target_sources(test_file PRIVATE ${PROJECT_SOURCE_DIR}/tools/hv_file.c)
target_link_libraries(test_file PRIVATE jpeg2000)
esajpip_add_test(output_file COMMAND test_file LABELS tools)

# The writer with a lowered LBox limit, to reach the switch to XLBox.
add_executable(test_writer test_writer.c)
esajpip_configure_target(test_writer)
target_link_libraries(test_writer PRIVATE jpeg2000)
esajpip_add_test(writer COMMAND test_writer LABELS tools)

# hv_served.c included, with open, fstat, read and malloc that misbehave on
# demand.
add_executable(test_served test_served.c)
esajpip_configure_target(test_served)
target_link_libraries(test_served PRIVATE jpeg2000)
esajpip_add_test(served
    COMMAND test_served ${PROJECT_SOURCE_DIR}/tests/vectors/j2k
    LABELS tools)

# The reader's framing, offsets and errors, byte by byte (hv_reader.c
# included, with allocations that fail on demand).
add_executable(test_reader test_reader.c)
esajpip_configure_target(test_reader)
target_link_libraries(test_reader PRIVATE jpeg2000)
esajpip_add_test(reader
    COMMAND test_reader ${PROJECT_SOURCE_DIR}/tests/vectors/j2k
    LABELS tools)

# hv_geometry against T.800 computed a second way (hv_geometry.c included,
# with allocations that fail on demand).
add_executable(test_geometry test_geometry.c)
esajpip_configure_target(test_geometry)
target_link_libraries(test_geometry PRIVATE jpeg2000)
esajpip_add_test(geometry COMMAND test_geometry LABELS tools)

# hv_walk's command line: options, exit status and the line of each outcome.
esajpip_add_test(walk_command
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/walk_test.sh
        $<TARGET_FILE:hv_walk>
        ${PROJECT_SOURCE_DIR}/tests/vectors/j2k
        ${CMAKE_CURRENT_BINARY_DIR}/walk
    LABELS tools cli)

# Exact outcomes at validation boundaries, including state and decoded values.
add_executable(test_rules test_rules.c)
esajpip_configure_target(test_rules)
target_link_libraries(test_rules PRIVATE jpeg2000)
esajpip_add_test(rules COMMAND test_rules LABELS tools)

# Owning local access: file lifetimes and injected allocation/I/O failures.
add_executable(test_local test_local.c)
esajpip_configure_target(test_local)
target_link_libraries(test_local PRIVATE jpeg2000)
esajpip_add_test(local_source
    COMMAND test_local ${PROJECT_SOURCE_DIR}/tests/vectors/j2k
    LABELS tools)

# Borrowed JPX layer mappings, independent of rendering and serving profiles.
add_executable(test_presentation test_presentation.c)
esajpip_configure_target(test_presentation)
target_link_libraries(test_presentation PRIVATE jpeg2000)
esajpip_add_test(presentation COMMAND test_presentation LABELS tools)
